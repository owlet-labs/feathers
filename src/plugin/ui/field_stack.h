/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/zone_select.h"

#include <memory>
#include <string>
#include <vector>

// A FIELD STACK: several controls of one thing in one captioned plate, the height of a knob's column.
//
//   ┌────────────┐
//   │ F  4.0kHz  │   drag-editable value rows from the top, each with its own control, its own modulation
//   │ W  50%     │   and its own tooltip; a Choice's selector, if there is one, goes at the foot
//   │ < Tilt >   │
//   │   FILTER   │
//   └────────────┘
//
// It is the look of a stacked `ModeValueCell` without the mode: the rows are the same compact boxes and the plate
// and caption are drawn the same way. It owns its rows and parents them; what each is bound to is its own.
namespace feathers::ui {

class FieldStack final : public visage::Frame, public TooltipSource {
public:
	explicit FieldStack(std::string caption);

	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	/// THE ROW'S OWN ANSWER for the control it holds (the stack's face is its rows' faces, as `ModeValueCell`'s is its selector's).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& control) const override;

	/// A VALUE ROW, below the ones added before it. `label` is the short inline word at its left.
	ValueBox& addBox(std::unique_ptr<ValueBox> box, std::string label);
	/// THE SELECTOR AT THE FOOT: one zone per name, over a Choice.
	ZoneSelect& setSelector(device::Modulatable& choice, std::vector<ZoneSelect::ZoneName> names);

	[[nodiscard]] size_t boxCount() const noexcept { return boxes_.size(); }
	[[nodiscard]] ValueBox& box(size_t i) const noexcept { return *boxes_[i]; }
	[[nodiscard]] ZoneSelect* selector() const noexcept { return select_.get(); }
	[[nodiscard]] const std::string& caption() const noexcept { return caption_; }

	void setFaceDiameter(float diameter);
	/// A ROW SWITCHED OFF DRAWS DIMMED AND TAKES NO HAND (the honesty rule), the selector likewise.
	void setBoxEnabled(size_t i, bool on);
	void setSelectorEnabled(bool on);
	/// A ROW NOT SHOWN AT ALL (a control that only another engine could enable).
	void setBoxShown(size_t i, bool shown);

	void draw(visage::Canvas& canvas) override;
	void resized() override;

private:
	std::string caption_;
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	std::unique_ptr<ZoneSelect> select_;
	float faceDiameter_ = Knob::kDiameter;
};

} // namespace feathers::ui
