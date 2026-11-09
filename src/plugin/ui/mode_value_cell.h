/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/knob.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/zone_select.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// **A MODE AND THE NUMBER THAT MODE READS, IN ONE CELL THAT LOOKS LIKE A ZONE KNOB — backlog IO addendum.**
//
// Groove wears the normal zone knob look — the value on top, the feel's
// name under it between two arrows, as IG's first Groove was (`58% / Swing 16`). But the PARAMETERS stay
// split (`Groove Mode`, `Groove Amount`, `Free Phase`): the zone look is presentation, and the reason the
// split was made — an amount can never roll into another feel — is not given back.
//
// **WHY NOT `ZoneCoordCell`.** Its box and its selector are two views of ONE coordinate: the selector names
// the region the number is in. Here they are different controls — the selector is the Choice, and the box
// is whichever control GATED ON that Choice is live now (Amount in Swing and Groove, Free Phase in Free).
// A cell that bound one control to both would be the rolling zone knob again. So this composes the same
// two parts `ZoneCoordCell` does, in the same Column arrangement and footprint, with the binding split:
//
// - **THE SELECTOR IS A `ZoneSelect` OVER THE CHOICE**, one zone per name; a stepped control's arrows stop
//   at its ends (`ZoneSelect::stepZone`), so nothing laps.
// - **THE BOX IS ONE `ValueBox` PER FOLLOWER**, each bound to its own control from construction, so the
//   digit drag and the modulation display are that control's own — no box is re-pointed at another param.
//   Exactly the live one is shown; none is where no follower is live, and the cell then prints the first
//   follower's own text for the mode ("straight"), muted, since no drag there would change the sound.
//
// Declared by the descriptor (`ParamDesc::modeCell`), seated by `GenericExpanded`.

namespace feathers::ui {

class ModeValueCell final : public visage::Frame, public TooltipSource {
public:
	/// THE CELL'S OWN SURFACE — its caption — is the MODE's, which is what the caption names. The box and
	/// the selector inside it answer for themselves. See `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&select_->control()); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &select_->control(), Road::Face }); }
	/// The selector's answer: it is the face over the mode.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override { return select_->modLookFor(c); }

	/// @param mode      the Choice the selector steps
	/// @param caption   drawn where a knob's caption goes
	/// @param names     the Choice's own names, one zone each
	/// @param relevant  whether a follower's param is live now — the panel's own gate walk
	ModeValueCell(device::Modulatable& mode, std::string caption, std::vector<ZoneSelect::ZoneName> names,
	              std::function<bool(size_t)> relevant);

	/// A control gated on the mode, and the box that drives it. The cell owns the box and parents it.
	ValueBox& addFollower(size_t param, std::unique_ptr<ValueBox> box);

	/// **SHOWS THE BOX OF THE FOLLOWER THAT IS LIVE NOW**, and reports whether that changed. The panel calls
	/// it from its layout pass; nothing here polls.
	bool refresh();

	/// The param the shown box drives, or -1 where none is live (Straight).
	[[nodiscard]] long activeParam() const noexcept { return active_; }
	[[nodiscard]] const std::vector<size_t>& followerParams() const noexcept { return params_; }
	[[nodiscard]] ValueBox* boxFor(size_t param) const;
	[[nodiscard]] ZoneSelect& select() const noexcept { return *select_; }

	void setFaceDiameter(float diameter);
	/// THE NARROWEST THIS CELL CAN BE and still print its widest mode name — the width a row rebalances toward, as
	/// `ZoneCoordCell::minimumWidth` is for a coordinate.
	[[nodiscard]] float minimumWidth() const;
	/// **GREYED WHEN THE DEVICE CANNOT BE HEARD** — `ZoneCoordCell::setEnabled`'s contract: the plate and caption dim
	/// and the selector offers no verb; the value box still takes the hand. The Shaper's Knee 0 is the user (IW).
	void setEnabled(bool on);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }
	void draw(visage::Canvas& canvas) override;
	void resized() override;

private:
	[[nodiscard]] float coordinateRowTop() const;
	[[nodiscard]] float nameRowTop() const;

	std::string caption_;
	std::unique_ptr<ZoneSelect> select_;
	std::function<bool(size_t)> relevant_;
	std::vector<size_t> params_;
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	long active_ = -2; ///< -2: never refreshed, so the first refresh always reports a change
	float faceDiameter_ = Knob::kDiameter;
	bool enabled_ = true;
};

} // namespace feathers::ui
