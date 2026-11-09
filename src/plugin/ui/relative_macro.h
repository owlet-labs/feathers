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

#include "backplane/device_host.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/edit_history.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// A RELATIVE MACRO'S TURN, APPLIED AS AN EDIT OF EVERY BASE IT IS ROUTED TO (backlog PQ).
//
// A Relative macro (`device::MacroType::Relative`) has no
// value. Its face is an endless encoder and a turn of it is an INCREMENT, in macro units — one unit is what
// a full sweep of an absolute macro's knob is worth. Each route from it moves its destination's STORED BASE
// by `amount × delta × span` (the span the engine scales that route by), on the message thread, as one undo
// step per gesture. The engine reads the macro as resting at 0 (`ParamSet::setMacroRelativeMask`), so
// nothing is added per block: the edit is the whole effect, and a reload keeps it because the base is
// already patch state.
//
// **WHAT EACH DESTINATION DOES WITH AN INCREMENT** (`RelativeRule`): a sound's Gamma carries into the
// unbounded coarse half (`SoundGamma`, to be confirmed); Theta and Phase and a `Unit::Degrees` device
// control wrap; everything else clamps to its range, and a stepped control steps — its running value is
// kept unrounded through the gesture, so small increments add up to a step rather than vanishing.
//
// HOST-AGNOSTIC IN SHAPE BUT FEATHERS' IN FACT: the gamma base and the orbit are Feathers' own patch state,
// so this takes Feathers' bridge. The rack has no relative road yet (it refuses the type).

namespace feathers::ui {

/// HOW A BASE TAKES AN INCREMENT.
enum class RelativeRule : uint8_t {
	Clamp,     ///< stops at `lo`/`hi`, and turning back moves off the end at once (the clamp is per increment)
	Wrap,      ///< periodic over [lo, hi)
	Unbounded, ///< accumulates without end
};

/// **ONE INCREMENT, PURE** — `base + step` under `rule` over [lo, hi]. Wrap folds into [lo, hi); the caller
/// folds Theta its own way (`std::remainder`, as its knob does).
[[nodiscard]] double relativeStep(double base, double step, double lo, double hi, RelativeRule rule);

/// **ONE GESTURE ON A RELATIVE MACRO.** `begin` snapshots every route from the macro and the base each one
/// reaches; `turn` moves them all; `end` closes their host gestures and records ONE undo step holding every
/// base that moved. Lives no longer than the drag; holds no widget.
class RelativeMacroGesture {
public:
	RelativeMacroGesture(ParamBridge& bridge, backplane::DeviceHost& host) : bridge_(bridge), host_(host) {}
	~RelativeMacroGesture();

	RelativeMacroGesture(const RelativeMacroGesture&) = delete;
	RelativeMacroGesture& operator=(const RelativeMacroGesture&) = delete;

	/// Opens a gesture on header macro `macro`. Ends any gesture still open first (recording it).
	void begin(size_t macro);
	/// `delta` macro units — positive clockwise. Nothing outside a gesture.
	void turn(double delta);
	/// Closes the gesture and records one step into `history` (null records nothing), titled "Turn".
	void end(EditHistory* history);

	[[nodiscard]] bool open() const noexcept { return open_; }
	/// How many destinations this gesture is moving — 0 for a macro routed nowhere.
	[[nodiscard]] size_t targetCount() const noexcept { return targets_.size(); }

private:
	enum class Kind : uint8_t { Param, GammaBase, Radius, Theta, Control };

	struct Target {
		Kind kind = Kind::Param;
		backplane::Destination destination;
		clap_id id = 0;          ///< Param
		size_t slot = 0;         ///< GammaBase, Radius, Theta
		DeviceInstanceId device = kNoInstance; ///< Control
		size_t control = 0;      ///< Control
		double perUnit = 0.0;    ///< amount × span: how far one macro unit moves this base
		double lo = 0.0;
		double hi = 0.0;
		bool stepped = false;
		RelativeRule rule = RelativeRule::Clamp;
		double before = 0.0;     ///< the base at `begin`
		double running = 0.0;    ///< unrounded, ruled
		double written = 0.0;    ///< what was last written (rounded when stepped)
	};

	void collect(size_t macro);
	[[nodiscard]] double read(const Target& t) const;
	void write(const Target& t, double value);
	void openHostGesture(const Target& t);
	void closeHostGesture(const Target& t);

	ParamBridge& bridge_;
	backplane::DeviceHost& host_;
	std::vector<Target> targets_;
	bool open_ = false;
};

} // namespace feathers::ui
