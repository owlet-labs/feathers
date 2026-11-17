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



#include "plugin/ui/eroder_flow.h"

#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

constexpr float kLabelTracking = 0.0f;
constexpr float kArrow = 11.0f;
constexpr float kLabelSize = 9.0f;

std::string lower(const char* s) {
	std::string out = s;
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

} // namespace

EroderFlow::Path EroderFlow::pathFor(const ero::Row& row) {
	const auto at = [&](ero::Param p) { return row[static_cast<size_t>(p)]; };
	const int modeAt = std::clamp(static_cast<int>(at(ero::Param::Mode) + 0.5f), 0, static_cast<int>(ero::Mode::kCount) - 1);
	const auto mode = static_cast<ero::Mode>(modeAt);
	const bool emphasised = at(ero::Param::EmphAmount) != 0.0f;
	const bool inLoop = at(ero::Param::EmphInLoop) >= 0.5f;
	const bool comb = mode == ero::Mode::Comb;
	const bool inner = comb || at(ero::Param::Resonance) != 0.0f;
	const bool outer = at(ero::Param::Loop) > 0.0f;
	const bool driven = at(ero::Param::FbTap) >= 0.5f;

	Path path;
	const auto add = [&](std::string label, bool lit, bool beneath = false) {
		path.stages.push_back({ std::move(label), lit, beneath });
		return path.stages.size() - 1;
	};
	const size_t input = add("in", true);
	if (!inLoop) {
		add("emph", emphasised);
	}
	const size_t sum = add("\xE2\x8A\x95", inner || outer);
	if (inLoop) {
		add("emph", emphasised);
	}
	add("smear", at(ero::Param::Smear) > 0.0f);
	size_t engineEnd = 0;
	if (mode == ero::Mode::Etch) {
		add("A", at(ero::Param::SlotAAmount) > 0.0f && at(ero::Param::SlotAMix) > 0.0f);
		const bool parallel = at(ero::Param::SlotRoute) >= 0.5f;
		engineEnd = add("B", at(ero::Param::SlotBAmount) > 0.0f && at(ero::Param::SlotBMix) > 0.0f, parallel);
	}
	else {
		engineEnd = add(lower(ero::kModeNames[modeAt]), true);
	}
	const size_t drive = add("drive", at(ero::Param::Drive) > 0.0f);
	add("restore", emphasised && at(ero::Param::EmphRestore) >= 0.5f);
	const size_t hp = add("hp", static_cast<double>(at(ero::Param::HpWet)) > ero::kHpWetLog2Min);
	add("mix", true);
	// SUB THRU GIVES BACK, AFTER HP WET, WHAT HP WET TOOK OF THE INPUT (`Eroder::processStereo`): nothing without a filter,
	// and in the Comb nothing while the dry is whole (Blend off) — the window's enable rule.
	const bool filtering = static_cast<double>(at(ero::Param::HpWet)) > ero::kHpWetLog2Min;
	const bool crossfades = !comb || at(ero::Param::Blend) >= 0.5f;
	path.bypass = { input, hp, at(ero::Param::SubThru) >= 0.5f && filtering && crossfades };
	path.returns.push_back({ driven ? drive : engineEnd, sum, inner, false });
	path.returns.push_back({ drive, sum, outer, true });
	return path;
}

EroderFlow::EroderFlow(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	// THE FRAME IS NOT A CONTROL: presses pass through to the chips seated in it.
	setIgnoresMouseEvents(true, true);
	(void)update();
}

void EroderFlow::seat(visage::Frame& inLoop, visage::Frame& tap, visage::Frame& restore, visage::Frame& subThru) {
	inLoop_ = &inLoop;
	tap_ = &tap;
	restore_ = &restore;
	subThru_ = &subThru;
	for (visage::Frame* f : { inLoop_, tap_, restore_, subThru_ }) {
		addChild(f);
	}
	resized();
}

std::vector<std::string> EroderFlow::labelsForTest() const {
	std::vector<std::string> out;
	for (const Stage& s : path_.stages) {
		out.push_back(s.label);
	}
	return out;
}

float EroderFlow::labelWidth(const std::string& text) const {
	return trackedWidth(font(kLabelSize), text, kLabelTracking);
}

bool EroderFlow::update() {
	ero::Row row {};
	(void)readModulatedRow(host_, device_, ero::kDescriptor, row);
	Path next = pathFor(row);
	if (computed_ && next == path_) {
		return false;
	}
	path_ = std::move(next);
	computed_ = true;
	++recomputes_;
	measure();
	resized();
	redraw();
	return true;
}

void EroderFlow::measure() {
	// COLUMNS LEFT TO RIGHT: a stage beneath shares the column before it. The arrows take `kArrow` between columns, and the
	// whole run is centred in the frame.
	columns_.assign(path_.stages.size(), 0.0f);
	halves_.assign(path_.stages.size(), 0.0f);
	float total = 0.0f;
	std::vector<float> widths(path_.stages.size(), 0.0f);
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		widths[i] = labelWidth(path_.stages[i].label);
		if (path_.stages[i].beneath && i > 0) {
			widths[i] = std::max(widths[i], widths[i - 1]);
			widths[i - 1] = widths[i];
		}
	}
	size_t columnsCount = 0;
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		if (!path_.stages[i].beneath) {
			total += widths[i];
			++columnsCount;
		}
	}
	total += kArrow * static_cast<float>(columnsCount > 0 ? columnsCount - 1 : 0);
	float x = (static_cast<float>(width()) - total) * 0.5f;
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		if (path_.stages[i].beneath && i > 0) {
			columns_[i] = columns_[i - 1];
			halves_[i] = halves_[i - 1];
			continue;
		}
		columns_[i] = x + widths[i] * 0.5f;
		halves_[i] = widths[i] * 0.5f;
		x += widths[i] + kArrow;
	}
}

void EroderFlow::resized() {
	measure();
	// THE CHIPS, a row along the foot of the frame, each as near the part of the drawing it moves as the row allows: In Loop
	// under the sum, Driven under the Drive, Restore under its own stage. Kept apart and inside the frame.
	if (inLoop_ == nullptr) {
		return;
	}
	const float w = static_cast<float>(width());
	const float chipW = 72.0f;
	const auto centreOf = [&](const char* label, size_t fallback) {
		for (size_t i = 0; i < path_.stages.size(); ++i) {
			if (path_.stages[i].label == label) {
				return columns_[i];
			}
		}
		return columns_.empty() ? static_cast<float>(fallback) : columns_[fallback % columns_.size()];
	};
	const float want[4] = { centreOf("\xE2\x8A\x95", 2), centreOf("drive", 6), centreOf("restore", 7), centreOf("hp", 8) };
	constexpr size_t kChips = 4;
	float left = kPad;
	visage::Frame* chips[kChips] = { inLoop_, tap_, restore_, subThru_ };
	for (size_t i = 0; i < kChips; ++i) {
		const float room = chipW * static_cast<float>(kChips - i) + 4.0f * static_cast<float>(kChips - 1 - i);
		float x = std::min(std::max(left, want[i] - chipW * 0.5f), w - kPad - room);
		x = std::max(x, left);
		chips[i]->setBounds(x, kChipTop, chipW, 16.0f);
		left = x + chipW + 4.0f;
	}
}

void EroderFlow::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.roundedRectangleBorder(0.5f, 0.5f, w - 1.0f, h - 1.0f, 5.0f, 1.0f);

	const float cy = 33.0f;
	const float rowGap = 5.5f;
	const visage::Font& face = font(kLabelSize);
	const auto yOf = [&](size_t i) { return path_.stages[i].beneath ? cy + rowGap : (i + 1 < path_.stages.size() && path_.stages[i + 1].beneath ? cy - rowGap : cy); };

	// LABELS AND THE ARROWS BETWEEN COLUMNS.
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		const Stage& s = path_.stages[i];
		canvas.setColor(s.lit ? kInk : withAlpha(kMuted, 0.4f));
		canvas.text(s.label, face, visage::Font::kCenter, columns_[i] - halves_[i] - 4.0f, yOf(i) - 5.5f, 2.0f * halves_[i] + 8.0f, 11.0f);
		if (s.beneath || i + 1 >= path_.stages.size()) {
			continue;
		}
		// THE ARROW LEAVES THE LAST OF THIS COLUMN AND ENTERS THE FIRST OF THE NEXT.
		const float x0 = columns_[i] + halves_[i] + 2.0f;
		const float x1 = columns_[i + 1 < path_.stages.size() ? i + 1 : i] - halves_[i + 1] - 2.0f;
		const bool lit = s.lit && path_.stages[i + 1].lit;
		canvas.setColor(lit ? withAlpha(kInk, 0.7f) : withAlpha(kMuted, 0.35f));
		canvas.segment(x0, cy, x1, cy, 1.0f, true);
		canvas.segment(x1 - 2.5f, cy - 2.0f, x1, cy, 1.0f, true);
		canvas.segment(x1 - 2.5f, cy + 2.0f, x1, cy, 1.0f, true);
	}

	// THE SUB THRU ROAD UNDER THE LINE: off the input, below the labels, and up into the arrow after HP Wet.
	{
		const Bypass& b = path_.bypass;
		const float xFrom = columns_[b.from];
		const float xTo = columns_[b.to] + halves_[b.to] + kArrow * 0.5f;
		const float yTo = cy + 8.0f;
		canvas.setColor(b.lit ? withAlpha(kValue, 0.95f) : withAlpha(kMuted, 0.3f));
		canvas.segment(xFrom, yTo - 1.0f, xFrom, kBypassY, 1.2f, true);
		canvas.segment(xFrom, kBypassY, xTo, kBypassY, 1.2f, true);
		canvas.segment(xTo, kBypassY, xTo, yTo, 1.2f, true);
		canvas.segment(xTo - 2.5f, yTo + 3.0f, xTo, yTo, 1.2f, true);
		canvas.segment(xTo + 2.5f, yTo + 3.0f, xTo, yTo, 1.2f, true);
	}

	// THE RETURNS OVER THE LINE: up from the stage they leave, across, and down into the sum, the outer one over the inner.
	for (const Return& r : path_.returns) {
		const float top = r.outer ? 5.0f : 11.0f;
		const float xFrom = columns_[r.from] + halves_[r.from] + (r.outer ? -1.0f : -4.0f);
		const float xTo = columns_[r.to] + (r.outer ? 2.5f : -2.5f);
		const float yFrom = cy - 8.0f;
		const float yTo = cy - 8.0f;
		canvas.setColor(r.lit ? withAlpha(r.outer ? tokens::kGammaAxis : kValue, 0.95f) : withAlpha(kMuted, 0.3f));
		canvas.segment(xFrom, yFrom, xFrom, top, 1.2f, true);
		canvas.segment(xFrom, top, xTo, top, 1.2f, true);
		canvas.segment(xTo, top, xTo, yTo, 1.2f, true);
		canvas.segment(xTo - 2.5f, yTo - 3.0f, xTo, yTo, 1.2f, true);
		canvas.segment(xTo + 2.5f, yTo - 3.0f, xTo, yTo, 1.2f, true);
	}
}

} // namespace feathers::ui
