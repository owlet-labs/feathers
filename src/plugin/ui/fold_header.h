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

#pragma once

#include "plugin/ui/theme.h"

#include <functional>
#include <string>
#include <utility>
#include <visage/ui.h>

namespace feathers::ui {

/// **A SECTION'S FOLD AFFORDANCE: ONE ROW, A CHEVRON AND THE SECTION'S NAME.** Pressing anywhere on it folds or unfolds the section it
/// heads (`onToggle`, the owner's, which also keeps the state; this frame only draws it). The chevron is geometry, not a character, and
/// points down while the section is open and right while it is folded. Holds no widget of the section: the owner shows and hides those.
class FoldHeader final : public visage::Frame {
public:
	explicit FoldHeader(std::string label) : label_(std::move(label)) {
		onMouseDown() = [this](const visage::MouseEvent&) {
			if (onToggle) {
				onToggle();
			}
		};
	}

	/// The hand pressed the row. The owner flips its state and calls `setFolded`.
	std::function<void()> onToggle;

	[[nodiscard]] bool folded() const noexcept { return folded_; }
	/// What the chevron shows. A change repaints.
	void setFolded(bool folded) {
		if (folded != folded_) {
			folded_ = folded;
			redraw();
		}
	}
	[[nodiscard]] const std::string& label() const noexcept { return label_; }

	void draw(visage::Canvas& canvas) override {
		const float h = static_cast<float>(height());
		const float mid = h * 0.5f;
		const float cx = 6.0f;
		const float s = 3.0f;
		canvas.setColor(kMuted);
		if (folded_) {
			canvas.segment(cx - 1.0f, mid - s, cx + s - 1.0f, mid, 1.4f, true);
			canvas.segment(cx - 1.0f, mid + s, cx + s - 1.0f, mid, 1.4f, true);
		}
		else {
			canvas.segment(cx - s, mid - 1.0f, cx, mid + s - 1.0f, 1.4f, true);
			canvas.segment(cx + s, mid - 1.0f, cx, mid + s - 1.0f, 1.4f, true);
		}
		drawTracked(canvas, label_, font(kControlSize), kControlTracking, 14.0f, 0.0f, h);
	}

private:
	std::string label_;
	bool folded_ = false;
};

} // namespace feathers::ui
