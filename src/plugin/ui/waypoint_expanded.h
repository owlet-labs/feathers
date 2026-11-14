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

#include "plugin/ui/widget_registry.h"
#include "fx/waypoint/waypoint_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <memory>
#include <vector>

// THE WAYPOINT FILTER'S EXPANDED VIEW — every waypoint at once.
//
// WHAT MAKES IT WORTH OPENING, rather than being the card drawn larger: the CARD shows one
// waypoint's macros, because a knob row has three cells and there are four waypoints. **The panel
// shows all sixteen per-waypoint controls together**, so a hand comparing waypoint 2 against
// waypoint 3 can see both instead of selecting between them. That is a different job, not a bigger
// version of the same one — which is the test `device.h` sets for declaring `hasExpanded` at all.
//
// A ROW PER WAYPOINT, a column per field. The grid is the point: reading DOWN a column compares one
// quantity across the four states, which is exactly the question the morph asks.

namespace feathers::ui {

class WaypointExpanded final : public ExpandedView {
public:
	WaypointExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                 Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	WaypointExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	/// Backlog EI: WIDE WITHOUT LIMIT, since the plane and the knob columns share out the width; the
	/// height is capped at the natural size plus one `kRowGap`, since the plane and the rows are fixed.
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kPad = 12.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// FOUR COLUMNS: cutoff, resonance, and the two type-following macros. One per field of a
	/// waypoint, which is what makes a column comparable down its length.
	static constexpr size_t kColumns = 4;
	static constexpr float kCell = 118.0f;
	/// THE PLANE, LARGER THAN THE CARD'S because this is where a hand does the aiming. OWLTT measured
	/// the lesson: at 48px its graph's hit-bands claimed two thirds of the vertical pixels and the
	/// result felt clunky — targets that cannot be aimed rather than too many controls.
	static constexpr float kPlaneHeight = 190.0f;

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	/// EVERY CONTROL THIS PANEL REACHES — the same claim the card makes, asked of the same kind of
	/// list, so the reachability guard can hold the panel to it too.
	[[nodiscard]] std::vector<fx::waypoint::Param> addressedParamsForTest() const;

private:
	[[nodiscard]] DeviceControl& control(fx::waypoint::Param p) const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	/// The device-wide choices that are not knobs: the filter type's list, and two chips.
	std::unique_ptr<ValueBox> type_;
	std::unique_ptr<SwitchChip> keytrack_;
	std::unique_ptr<SwitchChip> poly_;
	std::vector<fx::waypoint::Param> knobParams_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
