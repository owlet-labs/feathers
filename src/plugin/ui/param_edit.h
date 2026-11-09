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
#include "plugin/ui/edit_history.h"

#include <memory>

// ONE HOST PARAMETER CHANGED, FOR THE EDITOR'S ONE HISTORY (backlog JB, 2026-10-11).
//
// THE PARAMETER ROAD RECORDED NOTHING. `ParamControl` recorded route amounts and left values to "the
// host's undo", and every menu pick on a parameter (the engine, Warp mode, the Resonate model) bracketed
// a host gesture and stopped there. The editor has had Ctrl/Cmd+Z on its own history since dff05099, so
// a Ctrl+Z after a cutoff drag walked back whatever OLDER edit was on our stack and skipped the drag —
// worse than no key at all. Now every hand on a parameter is one step here.
//
// **AN UNDO IS A HAND ON THE PARAMETER, AND THE HOST HEARS IT AS ONE** — Begin/Value/End through the
// bridge's ordinary road, the road a drag takes, so the DAW and its automation see the undo as an edit of
// the parameter rather than the value changing under them. `DeviceValueEdit` makes the same choice for a
// device's control.

namespace feathers::ui {

/// A RECORD OVER VALUES AND THE BRIDGE, no widget captured (edit_history.h's rule). The bridge outlives
/// the editor's history: `Gui::Impl` clears the history in its teardown.
class ParamValueEdit final : public EditRecord {
public:
	ParamValueEdit(ParamBridge& bridge, clap_id id, double before, double after, const char* verb = "Set")
	    : bridge_(bridge), id_(id), before_(before), after_(after), verb_(verb) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	/// A string literal from the call site, as every record's verb is.
	const char* verb() const override { return verb_; }

private:
	void write(double value) const {
		bridge_.beginGesture(id_);
		bridge_.setValue(id_, value);
		bridge_.endGesture(id_);
	}

	ParamBridge& bridge_;
	clap_id id_ = 0;
	double before_ = 0.0;
	double after_ = 0.0;
	const char* verb_ = "Set";
};

/// **A ONE-SHOT WRITE TO A PARAMETER — a menu pick, a step, a switch — AS ONE HOST GESTURE AND ONE UNDO
/// STEP.** Into the editor's history (`ParamBridge::editHistory`), and none for a write that moved nothing:
/// the bridge's own reading is compared, so a pick of the value already there costs no step. The gesture is
/// posted either way, which is what every one of these sites did before it recorded. True if it moved.
inline bool writeParamAsEdit(ParamBridge& bridge, clap_id id, double value, const char* verb = "Set") {
	const double before = bridge.value(id);
	bridge.beginGesture(id);
	bridge.setValue(id, value);
	bridge.endGesture(id);
	const double after = bridge.value(id);
	if (after == before) {
		return false;
	}
	if (EditHistory* history = bridge.editHistory(); history != nullptr) {
		history->record(std::make_unique<ParamValueEdit>(bridge, id, before, after, verb));
	}
	return true;
}

} // namespace feathers::ui
