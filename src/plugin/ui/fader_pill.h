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

/// **A FADER PILL: AN AMOUNT AND ITS POWER IN THE FOOTPRINT OF A SWITCH** (backlog IK, 2026-09-23).
///
/// It proved itself on the shaper's extras, and a large part of its appeal is that it is much
/// smaller than a regular knob. It began as a drawing and a gesture inside
/// `shaper_plot.h` (backlog II); it is its own widget because it now carries the whole modulation
/// surface a knob does — full modulation support with glow, modulated amount, modulated amount
/// when armed, etc. — and because the next thing asked of it is a stack of
/// three in a knob's cell, which wants a widget and not a function.
///
/// **TWO CONTROLS, TWO DESTINATIONS.** Amount and on/off were conflated under modulation
/// — with 0 meaning off, a routed amount on an extra that was off turned it
/// on. So a pill drives an AMOUNT and a POWER, and the device hears `on ? amount : 0`:
///   - a CLICK toggles the power, and the amount stays where it was (drawn dim while off);
///   - a sideways DRAG sets the amount, `kSpan` pixels for the whole range whatever the pill's width;
///   - with a source ARMED, the face glows and a drag sets how far that source reaches the AMOUNT,
///     and a power symbol appears at the right end (a small power button floating near it in
///     mod-assign mode) — a drag there sets how far it reaches the POWER. Double-click
///     unroutes, right-click lists the inbound routes, Alt edits the value underneath — the knob's
///     gestures, asked of the same `device::Modulatable` the knob asks.
///
/// A power that is not a destination (a switch that reshapes a table) draws no symbol; an amount that
/// is not one does not glow. The honesty rule applied to a verb: a lit control offers a drag that works.
class FaderPill : public visage::Frame, public TooltipSource {
public:
	/// The power's tooltip over the power symbol, the amount's everywhere else — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point local) const override;
	/// **THE CAPTION AND THE NAME IT STANDS FOR** (`ui::LabelFace`) — the amount's declared name, away from the power
	/// symbol; cut when the caption is wider than the room `draw` gives it.
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point local) const override;
	/// Both of its controls are on its face: the fill is the amount, the power symbol the switch.
	void offeredControls(std::vector<Offered>& out) const override {
		out.push_back({ &amount_, Road::Face });
		if (power_ != nullptr) {
			out.push_back({ power_, Road::Face });
		}
	}
	/// What `draw` lights for each of its two controls, for the glow-honesty sweep (`ui::ControlFace::modLookFor`).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;

	static constexpr float kSlop = 3.0f;
	static constexpr float kSpan = 120.0f;
	/// THE KNOB'S ROUTE CONSTANTS, so an amount routed on a pill feels like one routed on a knob.
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;

	FaderPill(device::Modulatable& amount, device::Modulatable& power, std::string caption);
	/// **AMOUNT ONLY** (the Spectral window's stacks): no power of its own, so a click does nothing and the
	/// fill is always lit; the pill greys by `setActive`, like a knob, when its section is off. The section keeps its chip.
	FaderPill(device::Modulatable& amount, std::string caption);
	[[nodiscard]] bool amountOnly() const noexcept { return power_ == nullptr; }

	/// GREY WHEN THE DEVICE'S SIGNAL DOES NOT REACH THIS — `Knob::setActive`'s meaning: not "hands off",
	/// only "not in the sound". The control still takes the hand.
	void setActive(bool active);
	[[nodiscard]] bool active() const noexcept { return active_; }
	/// Called with a spoken line after a gesture changes something ("Sub on", "Sub 60%").
	void setAnnouncer(std::function<void(const std::string&)> announce) { announce_ = std::move(announce); }
	/// **A READOUT AT THE RIGHT END, THE CAPTION MOVED TO THE LEFT** — the phaser's LFO pill prints its rate ("LFO"
	/// left, "0.30 Hz" right, as the mock draws it), the one place a card shows a control whose own box lives in the
	/// window. Empty (the default): the caption alone, centred, as every other pill. Repaints only when it changes.
	void setReadout(std::string readout) {
		if (readout != readout_) {
			readout_ = std::move(readout);
			redraw();
		}
	}
	[[nodiscard]] const std::string& caption() const noexcept { return caption_; }
	[[nodiscard]] const std::string& readout() const noexcept { return readout_; }
	/// **THE CAPTION FITS THE PILL** at rest (no readout), as `draw` centres it — a caption wider than the pill is
	/// drawn off both ends and reads as a fragment ("eedbac").
	[[nodiscard]] bool captionFits() const;

	void draw(visage::Canvas& canvas) override;
	void resized() override { refreshWatch(); }

	//-- test seams ---------------------------------------------------------------------------//
	[[nodiscard]] bool hoveringForTest() const noexcept { return hovering_; }
	[[nodiscard]] bool powerGlyphShownForTest() const { return powerGlyphShown(); }
	[[nodiscard]] visage::Bounds powerGlyphForTest() const { return powerGlyph(); }
	[[nodiscard]] bool poweredForTest() const { return powered(); }
	/// The controls the pill is over (the drawer's cases bind a pill to its param by them); the power is null on an amount-only pill.
	[[nodiscard]] device::Modulatable& amountForTest() const { return amount_; }
	[[nodiscard]] device::Modulatable* powerForTest() const { return power_; }
	/// Whether a route may be armed onto the pill's amount (the answer a press asks, Alt up).
	[[nodiscard]] bool mappableForTest() const { return mappable(amount_, false); }
	/// THE POWER'S "I AM MODULATED" RING — routed and not armed (the power, 'active', needs a
	/// modulated-amount display too).
	[[nodiscard]] bool powerRingShownForTest() const { return powerRingShown(); }
	/// **THE CONTROLS' PANEL STATE CHANGED**: re-subscribe now, off the old state's watcher list (backlog PM).
	void modulationChanged() { refreshWatch(); }

private:
	enum class Target { None, Amount, Power };

	[[nodiscard]] bool powered() const;
	[[nodiscard]] bool powerGlyphShown() const;
	[[nodiscard]] bool powerRingShown() const;
	[[nodiscard]] visage::Bounds powerGlyph() const;
	[[nodiscard]] static bool mappable(const device::Modulatable& control, bool altDown);
	void setHover(bool over);
	void refreshWatch();
	void say(const std::string& line) const {
		if (announce_) {
			announce_(line);
		}
	}

	device::Modulatable& amount_;
	device::Modulatable* power_ = nullptr; ///< null: amount only
	std::string caption_;
	std::string readout_;
	std::function<void(const std::string&)> announce_;
	bool active_ = true;
	bool hovering_ = false;

	// THE PRESS IN FLIGHT.
	Target target_ = Target::None;
	bool routing_ = false; ///< the press is setting a route's amount, not a value
	bool dragged_ = false;
	float startX_ = 0.0f;
	float startY_ = 0.0f;
	double startValue_ = 0.0;
	float anchorAmount_ = 0.0f;

	/// THE MODULATION SUBSCRIPTION — `ValueBox::refreshWatch`'s, for the same reason: the glow and the
	/// live amount must repaint when the panel state moves, not when the hand next does.
	Watch watch_;
	const ModState* watched_ = nullptr;
};

} // namespace feathers::ui
