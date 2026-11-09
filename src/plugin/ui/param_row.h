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
#include "plugin/ui/widget_registry.h"

#include <visage/ui.h>

// One parameter as a bare horizontal slider.
//
// Deliberately plain: it exists to prove that every one of the 24 parameters can be read and written
// through the bridge, and it becomes the debug panel behind FEATHERS_DEBUG_PANEL once the real
// surfaces exist — the cheapest param-surface smoke test available.
//
// In its own file rather than tucked inside gui.cpp because a test needs to construct one. Nothing
// here wants a window: give it bounds and hand it mouse events, and it drives the bridge.

namespace feathers::ui {

class ParamRow : public visage::Frame {
public:
	ParamRow(ParamBridge& bridge, WidgetRegistry& registry, const ParamDesc& desc);

	void draw(visage::Canvas& canvas) override;

	const ParamDesc& desc() const noexcept { return desc_; }

private:
	/// **THE SUBSCRIPTION, HELD** — see `ui::Watch`. Declared LAST so it is destroyed FIRST, which
	/// releases into a list that is still alive.
	Watch watch_;
	clap_id id() const noexcept { return static_cast<clap_id>(desc_.id); }
	float normalized(double value) const noexcept;
	void applyFrom(const visage::MouseEvent& e);

	ParamBridge& bridge_;
	const ParamDesc& desc_;
};

} // namespace feathers::ui
