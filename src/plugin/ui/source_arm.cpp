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

#include "plugin/ui/source_arm.h"

#include "plugin/ui/theme.h"

namespace feathers::ui {

namespace {
constexpr float kLabelGap = 4.0f;

} // namespace

SourceArm::SourceArm(ModState& mod, device::Source source, std::string label)
    : mod_(mod), fixedSource_(source), label_(std::move(label)) {
	onMouseDown() = [this](const visage::MouseEvent&) {
		// THE HOST FIRST — see `onPressed`. A press on the chip is a press on the card it sits on.
		if (onPressed) {
			onPressed();
		}
		// STICKY AND EXCLUSIVE — the mock's rule: *"mapping stays STICKY — route several in one
		// session"*. Pressing the armed arm disarms; pressing another moves the focus and stays armed,
		// so a hand can route from one source to several controls without re-arming between them.
		//
		// ASKED, NOT REMEMBERED — see `source()`. Both reads in this lambda are the same call, so a
		// press cannot compare one moment's source against another's.
		// `this->` BECAUSE THE CONSTRUCTOR'S PARAMETER IS ALSO CALLED `source`, and unqualified it
		// would name that copy — the exact staleness this call exists to end, reintroduced by a
		// shadow. The compiler caught it; the qualification is what keeps it caught.
		const device::Source now = this->source();
		if (mod_.armed() && mod_.source() == now) {
			mod_.arm(false);
			return;
		}
		mod_.focus(now);
		mod_.arm(true);
	};
	// THE ARROW REDRAWS WHEN THE FOCUS MOVES, or arming one would leave the last one still lit.
	watch_ = mod_.watch(this);
}

SourceArm::~SourceArm() {
	// **WHOEVER REGISTERS, UNREGISTERS.** The watcher list holds raw frame pointers and `notify()`
	// walks them; an arm that died while still on it is the crash once hit on the arrow. Its own
	// destructor is the only place that cannot be forgotten by a distant owner.
	// (the token releases this now — see `ui::Watch`)
}

float SourceArm::widthFor(const std::string& label) {
	const float glyph = kArmArrowSize + 6.0f;
	if (label.empty()) {
		return glyph;
	}
	return SourceArm::kChipInset + glyph + kLabelGap + trackedWidth(font(kKnobLabelSize), label, kKnobLabelTracking)
	       + SourceArm::kChipInset;
}

float SourceArm::preferredWidth() const { return widthFor(label_); }

visage::Bounds SourceArm::glyphBounds() const {
	const float glyph = kArmArrowSize + 6.0f;
	const float top = y() + (static_cast<float>(height()) - glyph) * 0.5f;
	if (label_.empty()) {
		return { x() + (static_cast<float>(width()) - glyph) * 0.5f, top, glyph, glyph };
	}
	return { x() + SourceArm::kChipInset, top, glyph, glyph };
}

void SourceArm::draw(visage::Canvas& canvas) {
	const bool lit = mod_.armed() && mod_.source() == source();

	// A BORDERED CHIP WHEN IT CARRIES A NAME, a bare glyph when it does not. The border is what says
	// the word and the arrow are ONE button; an arrow floating beside a word reads as a caption.
	const float glyph = kArmArrowSize + 6.0f;
	float cx = static_cast<float>(width()) * 0.5f;
	if (!label_.empty()) {
		canvas.setColor(withAlpha(lit ? kModulation : kLine, lit ? 0.95f : 0.75f));
		canvas.roundedRectangleBorder(0.0f, 1.0f, static_cast<float>(width()),
		                              static_cast<float>(height()) - 2.0f, kCardCorner, 1.0f);
		cx = SourceArm::kChipInset + glyph * 0.5f;
	}

	// THE SAME MARK FEATHERS USES, from the floor rather than redrawn here — `drawArmArrow` replaced
	// three divergent copies and a fourth of mine, and is why the two hosts cannot drift again.
	canvas.setColor(lit ? kModulation : kMuted);
	drawArmArrow(canvas, cx, static_cast<float>(height()) * 0.5f, kArmArrowSize);

	if (!label_.empty()) {
		// **A CHIP NARROWER THAN ITS NAME PRINTS THE NAME'S LAST WORD** — "Mod 3" as "3" in a grid cell
		// (`Traits::outputGridColumns`). A flowed chip is always as wide as its name, so this never fires there.
		std::string shown = label_;
		if (static_cast<float>(width()) < widthFor(label_)) {
			if (const size_t space = label_.find_last_of(' '); space != std::string::npos) {
				shown = label_.substr(space + 1);
			}
		}
		canvas.setColor(lit ? kModulation : kMuted);
		drawTracked(canvas, shown.c_str(), font(kKnobLabelSize), kKnobLabelTracking,
		            SourceArm::kChipInset + glyph + kLabelGap, 0.0f, static_cast<float>(height()));
	}
}

namespace {
/// EVERY DECLARED OUTPUT'S CHIP WIDTH, IN ORDER — through `widthFor`, so what is reserved and what is
/// drawn are one measurement of one font. A device that names an output nothing still gets a chip
/// wide enough for the arrow, which is what `widthFor("")` answers.
///
/// **A LABEL, WHERE THE HOST HANDS ONE, IN PLACE OF THE DECLARED NAME** — the chip prints what this
/// instance calls the output (`SourceArm`'s label), so the width reserved for it must be that word's.
std::vector<float> chipWidthsOf(const device::Descriptor& desc, std::span<const std::string> labels, uint64_t shown) {
	std::vector<float> widths;
	widths.reserve(desc.outputCount());
	for (size_t i = 0; i < desc.outputs.size(); ++i) {
		if (i < 64 && ((shown >> i) & 1u) == 0u) {
			widths.push_back(0.0f); // HIDDEN: no room, an empty rectangle
			continue;
		}
		const char* declared = desc.outputs[i].name;
		widths.push_back(SourceArm::widthFor(i < labels.size() ? labels[i]
		                                                       : std::string(declared != nullptr ? declared : "")));
	}
	return widths;
}
/// **THE GRID ROAD (`Traits::outputGridColumns`)** — equal cells, `columns` to a row, in declared order. The same
/// padding the flow uses at the strip's two ends and between chips, so a grid card's foot lines up with every other
/// card's. Counting and placing are one walk here too: rows are `ceil(n / columns)` whatever the width, and a cell
/// is shown when its ROW band fits the strip (the flow's DOWN test) and it has any width at all.
size_t layoutOutputGrid(size_t count, size_t columns, visage::Bounds strip, std::vector<visage::Bounds>* out) {
	if (out != nullptr) {
		out->clear();
	}
	if (count == 0 || columns == 0) {
		return 0;
	}
	const size_t rows = (count + columns - 1) / columns;
	if (out == nullptr) {
		return rows;
	}
	const float usable = std::max(0.0f, strip.width() - 2.0f * kCardOutputChipPadX);
	const float cols = static_cast<float>(columns);
	const float cell = std::max(0.0f, (usable - (cols - 1.0f) * kCardOutputChipGap) / cols);
	out->reserve(count);
	for (size_t i = 0; i < count; ++i) {
		const size_t row = i / columns;
		const size_t col = i % columns;
		const bool fitsDown = static_cast<float>(row + 1) * kCardOutputStripHeight <= strip.height() + 0.01f;
		if (!fitsDown || cell < SourceArm::widthFor("")) {
			out->push_back({});
			continue;
		}
		const float top = strip.y() + static_cast<float>(row) * kCardOutputStripHeight;
		out->push_back({ strip.x() + kCardOutputChipPadX + static_cast<float>(col) * (cell + kCardOutputChipGap),
		                 top + (kCardOutputStripHeight - SourceArm::kHeight) * 0.5f, cell, SourceArm::kHeight });
	}
	return rows;
}
} // namespace

size_t outputChipRowsFor(const device::Descriptor& desc, float stripWidth, std::span<const std::string> labels,
                         uint64_t shown) {
	if (desc.traits.outputGridColumns > 0) {
		return layoutOutputGrid(desc.outputCount(), desc.traits.outputGridColumns, {}, nullptr);
	}
	const std::vector<float> widths = chipWidthsOf(desc, labels, shown);
	return outputChipRows(widths, stripWidth);
}

std::vector<visage::Bounds> outputChipBoundsFor(const device::Descriptor& desc, visage::Bounds strip,
                                                std::span<const std::string> labels, uint64_t shown) {
	std::vector<visage::Bounds> out;
	if (desc.traits.outputGridColumns > 0) {
		layoutOutputGrid(desc.outputCount(), desc.traits.outputGridColumns, strip, &out);
		return out;
	}
	const std::vector<float> widths = chipWidthsOf(desc, labels, shown);
	layoutOutputChips(widths, strip, SourceArm::kHeight, &out);
	return out;
}

} // namespace feathers::ui
