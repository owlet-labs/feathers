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

#include "device/modulatable.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/tooltip.h"

#include <functional>
#include <string>
#include <visage/ui.h>

namespace feathers::ui {

class ModState;

/// **A VERTICAL FADER: ONE CONTROL IN A STRIP BESIDE A PICTURE** (backlog NB, 2026-10-17).
///
/// It exists for OWLTT's Drive: the card has no room for another knob, so the control sits left of the graph as a
/// vertical bar. It is a bar as tall as the plot beside it, with the reading on top and the caption at the
/// foot, and it carries the whole modulation surface a knob does — `FaderPill`'s gestures turned on their side:
///   - an upward DRAG raises the value, `kSpan` pixels for the whole range whatever the bar's height;
///   - DOUBLE-CLICK resets to the declared default; a right-click lists the inbound routes;
///   - with a source ARMED, the border glows and a drag sets how far that source reaches (double-click unroutes);
///     Alt edits the value underneath.
///
/// THE FILL STARTS AT THE DEFAULT, not at the bottom: a Drive of -12..+24 dB rests at 0 dB, a third of the way up, and
/// a cut should read as a cut. A tick marks the rest.
class VerticalFader : public visage::Frame, public TooltipSource {
public:
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	/// WHAT `draw` LIGHTS (backlog QK, the glow-honesty sweep): the mapping border while the armed source may reach it, and a
	/// routed mark — the reach band, or the dot when the reach clamps or the route is another source's.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override {
		if (&c != &control_) {
			return {};
		}
		const device::ModDisplay d = control_.display();
		return { d.isDestination, d.isDestination && d.routable, d.isDestination && (d.amount != 0.0f || d.routedElsewhere) };
	}

	static constexpr float kSlop = 3.0f;
	static constexpr float kSpan = 160.0f;
	/// THE KNOB'S ROUTE CONSTANTS, so a route made on the bar feels like one made on a knob.
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;
	/// The reading's line at the top and the caption's at the foot; the track is what is between them.
	static constexpr float kTextHeight = 12.0f;

	VerticalFader(device::Modulatable& control, std::string caption);

	/// Called with a spoken line after a gesture changes something ("Drive +6.0 dB").
	void setAnnouncer(std::function<void(const std::string&)> announce) { announce_ = std::move(announce); }

	void draw(visage::Canvas& canvas) override;
	void resized() override { refreshWatch(); }

	[[nodiscard]] device::Modulatable& control() const noexcept { return control_; }
	[[nodiscard]] const std::string& caption() const noexcept { return caption_; }
	/// WHAT THE TOP OF THE BAR PRINTS — the number alone, signed, in the control's unit: a bar this narrow has no room
	/// for the unit, which the caption and the tooltip carry.
	[[nodiscard]] std::string readout() const;
	/// The track the fill runs in, in the bar's own coordinates — asked by `draw`, the drag and the cases.
	[[nodiscard]] visage::Bounds track() const;
	/// The y on the track of a 0..1 position along the control's range (1 at the top).
	[[nodiscard]] float yOf(double normalized) const;

private:
	[[nodiscard]] bool mappable(bool altDown) const { return !altDown && control_.display().routable; }
	void refreshWatch();
	void say(const std::string& line) const {
		if (announce_) {
			announce_(line);
		}
	}

	device::Modulatable& control_;
	std::string caption_;
	std::function<void(const std::string&)> announce_;

	// THE PRESS IN FLIGHT.
	bool pressed_ = false;
	bool routing_ = false; ///< the press is setting a route's amount, not a value
	bool dragged_ = false;
	float startY_ = 0.0f;
	double startValue_ = 0.0;
	float anchorAmount_ = 0.0f;

	/// THE MODULATION SUBSCRIPTION — `FaderPill::refreshWatch`'s, for the same reason: the glow and the live mark
	/// must repaint when the panel state moves, not when the hand next does.
	Watch watch_;
	const ModState* watched_ = nullptr;
};

} // namespace feathers::ui
