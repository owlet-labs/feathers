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

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <visage/ui.h>

// The mock's `#notif`: one line in the lower left saying what you are doing, which fades out when
// you stop. Bitwig's readout is the reference and the mock copies it deliberately — while a knob is
// moving, the value under the pointer is the one thing you want to read, and it is the one place a
// dark editor can put text without it becoming furniture.
//
// It is transient by design. Anything that needs to stay on screen belongs to a control.

namespace feathers::ui {

/// Somewhere to say what just happened, in one line.
///
/// An interface rather than the tray itself so a widget can be built, and tested, without one — the
/// controls take a pointer that is allowed to be null, because a knob that cannot announce itself
/// should still turn.
class Notifier {
public:
	virtual ~Notifier() = default;

	/// Main thread, from a control that the user just moved.
	virtual void say(std::string_view text) = 0;
};

class NotifyTray : public visage::Frame, public Notifier {
public:
	/// The clock is injectable so the fade can be tested without waiting for it. Left empty, it is
	/// the real one.
	using Clock = std::function<int64_t()>;
	/// HOW LONG A LINE HOLDS AND HOW LONG IT FADES. The tray's own are the mock's (`kHoldMs`,
	/// `kFadeMs`); the preset-loaded toast is the same widget at its own timings (`kToastTiming`),
	/// because a second widget that draws one line and fades it would be this one twice.
	struct Timing {
		int64_t holdMs;
		int64_t fadeMs;
	};
	explicit NotifyTray(Clock clock = {}, Timing timing = { kHoldMs, kFadeMs });

	void say(std::string_view text) override;

	/// Advance to now. Returns true when something changed that the screen has not got yet — the fade
	/// having moved, or a new message having arrived — which is what stops a still tray from being
	/// redrawn every frame for the life of the editor.
	bool update();

	void draw(visage::Canvas& canvas) override;

	/// 1 while held, falling to 0 across the fade, 0 once gone.
	float opacity() const noexcept;
	bool visible() const noexcept { return opacity() > 0.0f; }
	const std::string& text() const noexcept { return text_; }

	/// The mock's timings: `setTimeout(…, 1400)` and `transition: opacity .15s`.
	static constexpr int64_t kHoldMs = 1400;
	static constexpr int64_t kFadeMs = 150;
	/// **THE PRESET TOAST'S TIMING — "Loaded: <name>"**, popped up for 100 ms. 100 ms is under what
	/// most people can read, so it is read as the
	/// moment the line stands at full strength: 100 ms opaque, then a 600 ms fade that is still
	/// legible for most of its length. Both numbers are open to tightening.
	static constexpr Timing kToastTiming { 100, 600 };
	[[nodiscard]] Timing timing() const noexcept { return timing_; }

	/// The mock's `left:18px; bottom:12px`, and the box it draws.
	static constexpr float kInsetX = 18.0f;
	static constexpr float kInsetY = 12.0f;
	static constexpr float kPadX = 10.0f;
	static constexpr float kPadY = 4.0f;
	static constexpr float kHeight = 20.0f;

private:
	Clock clock_;
	Timing timing_;
	std::string text_;
	int64_t saidAt_ = 0;
	bool everSaid_ = false;
	float drawnOpacity_ = 0.0f;
	/// A message the screen has not been given yet. The other half of update()'s decision, and the
	/// half no clock can supply.
	bool textChanged_ = false;
};

} // namespace feathers::ui
