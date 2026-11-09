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

#include "plugin/gui_bridge.h"

#include <visage/ui.h>

#include <functional>
#include <string>

// One audition handle as a bare control, for the debug strip only — the same standing as
// OrientationRow, and for the same reason: the questions these ask are COMPARATIVE, and a comparison
// you can only make by restarting the host is a comparison nobody makes twice.
//
// Generic where OrientationRow is specific, because the handles that follow it are one number each
// with nothing in common but needing a hand on them. It reads and writes through two callbacks, so
// the widget knows nothing about what it is tuning and the bridge stays the only place the value
// lives — a slider holding its own copy is a slider that can disagree with the engine after an
// environment variable sets the value underneath it.
//
// NEVER A CLAP PARAMETER: these exist only to pin values experimentally from the UI. Added only under FEATHERS_DEBUG_PANEL, spending no id and touching
// nothing a patch saves.

namespace feathers::ui {

class TuningRow : public visage::Frame {
public:
	/// A control over a handful of named values rather than a continuous track.
	///
	/// TWO-VALUE BY DESIGN for the falloff: §7.1d is an A/B, not a sweep — power 1 is the expected
	/// answer, to be confirmed by ear — and a continuous exponent would invite tuning a number nobody
	/// intends to keep. Stepped also makes the readout unambiguous: you are on A or you are on B, and
	/// the label says which.
	struct Step {
		double value;
		const char* name;
	};

	/// `read` returns what the engine is running on, `write` sends a new value the way the engine will
	/// receive it. Both go through the bridge; neither is stored here.
	TuningRow(std::string label, std::vector<Step> steps, std::function<double()> read,
	          std::function<void(double)> write, std::function<void()> onChanged);

	void draw(visage::Canvas& canvas) override;

private:
	/// Which step is live, by nearest value — so a value that arrived from the environment and matches
	/// no step still shows the one it is closest to rather than showing nothing.
	size_t index() const;

	std::string label_;
	std::vector<Step> steps_;
	std::function<double()> read_;
	std::function<void(double)> write_;
	std::function<void()> onChanged_;
};

} // namespace feathers::ui
