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

#include "plugin/ui/stack_index_plot.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

/// The inset the dots keep from the frame's edge, so a dot at ±1 or at an end part is drawn whole.
constexpr float kInset = 5.0f;
constexpr float kDotRadius = 2.5f;

} // namespace

StackIndexPlot::StackIndexPlot(ParamBridge& bridge, WidgetRegistry& registry) : bridge_(bridge) {
	// EVERY PARAMETER THE PICTURE IS MADE OF, so automation and a hand on any of them redraw it.
	for (const ParamId id : { ParamId::StackCount, ParamId::StackShape, ParamId::StackShapeAmount,
	                          ParamId::StackFreePhase, ParamId::StackMap, ParamId::StackMapAmount,
	                          ParamId::StackCurve }) {
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), this));
	}
	// LAST — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

StackIndexSettings StackIndexPlot::settings() const {
	const auto value = [this](ParamId id) { return bridge_.value(static_cast<clap_id>(id)); };
	StackIndexSettings s;
	s.count = static_cast<int>(std::clamp(std::lround(value(ParamId::StackCount)), 1L, static_cast<long>(kMaxStack)));
	s.shape = static_cast<StackShape>(steppedIndex(value(ParamId::StackShape), kStackShapeNames.size()));
	s.shapeAmount = static_cast<float>(value(ParamId::StackShapeAmount));
	s.freePhase = value(ParamId::StackFreePhase);
	s.map = static_cast<StackMap>(steppedIndex(value(ParamId::StackMap), kStackMapNames.size()));
	s.mapAmount = static_cast<float>(value(ParamId::StackMapAmount));
	s.curve = static_cast<float>(value(ParamId::StackCurve));
	// Random Fresh deals each note its own draw; the patch's picture shows the fixed one in its place.
	s.freshSeed = kStackRandomSeed;
	return s;
}

std::vector<float> StackIndexPlot::indicesForTest() const {
	const StackIndexSettings s = settings();
	const StackValues values = stackIndexSpread(s);
	return std::vector<float>(values.begin(), values.begin() + s.count);
}

visage::Point StackIndexPlot::dotAt(int part, int count, float index) const {
	const float w = std::max(0.0f, static_cast<float>(width()) - 2.0f * kInset);
	const float h = std::max(0.0f, static_cast<float>(height()) - 2.0f * kInset);
	const float x = count <= 1 ? 0.5f : static_cast<float>(part) / static_cast<float>(count - 1);
	return { kInset + x * w, kInset + (1.0f - (std::clamp(index, -1.0f, 1.0f) + 1.0f) * 0.5f) * h };
}

visage::Point StackIndexPlot::dotForTest(int part) const {
	const StackIndexSettings s = settings();
	const StackValues values = stackIndexSpread(s);
	return dotAt(part, s.count, values[static_cast<size_t>(std::clamp(part, 0, kMaxStack - 1))]);
}

void StackIndexPlot::setEnabled(bool on) {
	if (enabled_ != on) {
		enabled_ = on;
		redraw();
	}
}

void StackIndexPlot::draw(visage::Canvas& canvas) {
	const StackIndexSettings s = settings();
	const StackValues values = stackIndexSpread(s);
	// THE ZERO LINE — the centre part's place, and where every part stands with nothing routed.
	const visage::Point zero = dotAt(0, 2, 0.0f);
	canvas.setColor(withAlpha(kLine, 0.8f));
	canvas.fill(kInset, zero.y, std::max(0.0f, static_cast<float>(width()) - 2.0f * kInset), 1.0f);
	// ONE DOT PER PART, and a stem from the zero line to it, so a part at 0 still shows as a dot on the line.
	const uint32_t ink = enabled_ ? kValue : kDisabled;
	for (int part = 0; part < s.count; ++part) {
		const visage::Point at = dotAt(part, s.count, values[static_cast<size_t>(part)]);
		canvas.setColor(withAlpha(ink, 0.5f));
		canvas.fill(at.x - 0.5f, std::min(at.y, zero.y), 1.0f, std::abs(at.y - zero.y));
		canvas.setColor(ink);
		canvas.circle(at.x - kDotRadius, at.y - kDotRadius, 2.0f * kDotRadius);
	}
}

const char* StackIndexPlot::tooltipAt(visage::Point) const {
	return "Each stacked oscillator's Stack Index, lowest pitch on the left, from -1 at the bottom to +1 at the top: "
	       "what a route from the Stack Index gives each one.";
}

} // namespace feathers::ui
