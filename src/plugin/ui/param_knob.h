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
#include "plugin/ui/knob.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/widget_registry.h"

#include <memory>

// A KNOB OVER ONE OF THIS PLUGIN'S OWN PARAMETERS — the host half of what `Knob` used to do itself.
//
// `Knob` had a constructor taking a `ParamBridge` and a `WidgetRegistry`, which built a
// `ParamControl` internally and subscribed it for redraws. Both of those are facts about a HOST
// PARAMETER SPACE, and a device's card has neither: a docked device's controls have no clap_id, so
// the registry could never key on them and that road wrote `(void)registry;`.
//
// So the knowledge moved out here and `Knob` kept only what is true of any control. This function is
// what the five call sites that wanted the old behaviour now say instead, and it does exactly what
// that constructor did — including for a null descriptor, which stays a real state rather than an
// error.

namespace feathers::ui {

/// Build a knob over a host parameter, and subscribe it for redraws.
///
/// A NULL DESCRIPTOR IS A KNOB WITH NOTHING BEHIND IT, drawn as a bare track — and it subscribes to
/// nothing, because there is no parameter for the registry to key on. That is the behaviour the
/// removed constructor had, preserved here rather than reasoned about again at five call sites.
inline std::unique_ptr<Knob> paramKnob(ParamBridge& bridge, WidgetRegistry& registry,
                                       const ParamDesc* desc, const char* label,
                                       Notifier* notifier = nullptr) {
	auto control = std::make_unique<ParamControl>(bridge, desc);
	// READ BEFORE THE MOVE, AND ONLY WHEN THERE IS SOMETHING TO READ. `ParamControl::id()`
	// dereferences its descriptor, so asking an unbound control for its id is a null dereference —
	// which is exactly what a first draft of this function did, and it segfaulted the one fixture
	// that builds a knob over the mock's Weather with no parameter behind it.
	const clap_id id = desc != nullptr ? control->id() : 0;
	auto knob = std::make_unique<Knob>(std::move(control), label, notifier);
	if (desc != nullptr) {
		knob->holdWatch(registry.watchToken(id, knob.get()));
	}
	return knob;
}

} // namespace feathers::ui
