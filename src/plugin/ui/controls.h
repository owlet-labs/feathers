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

#include "device/modulatable.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/widget_registry.h"

#include <visage/ui.h>

#include <limits>
#include <memory>
#include <span>
#include <string>

// The two controls the mock's Engine rail is built out of, both bound to one parameter.
//
// Generic rather than one-offs because the mock uses each of them several times over — three
// segmented choices and four steppers between the voice and pitch sections — and because a control
// that knows only "a parameter and its table entry" cannot drift from what the engine will accept.
// Neither reads a value it did not get from the bridge, and neither decides what a value means:
// stepping asks the parameter table for its range, and printing asks valueToText.

namespace feathers::ui {

/// A row of mutually exclusive options, one per entry in a stepped parameter's choice list — the
/// mock's `.seg`.
class SegmentedControl : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the strip — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(control_); }
	void offeredControls(std::vector<Offered>& out) const override {
		if (control_ != nullptr) {
			out.push_back({ control_, Road::Face });
		}
	}

	/// THE HOST-PARAMETER ROAD, which most of this editor's segmented controls still take: the
	/// Engine rail's three and the sound stack's. It builds a ParamControl internally and hands it to
	/// the one below, exactly as Knob's pair does.
	SegmentedControl(ParamBridge& bridge, WidgetRegistry& registry, const ParamDesc& desc,
	                 std::span<const char* const> names, Notifier* notifier = nullptr);

	/// THE OTHER ROAD: a segmented control over ANY device::Modulatable.
	///
	/// The same split Knob already has, and taken for the same reason one step further along. A
	/// swallowed device's controls have no clap_id — the filter's Poly and Keytrack are two of them —
	/// so a card built out of host parameters cannot draw them at all. Knob was generalised when the
	/// filter's knobs needed it; these two switches are the rest of that card, and they are choices
	/// rather than dials, which is why the generalisation has to happen twice rather than once.
	///
	/// NO REGISTRY WATCH ON THIS ROAD, and it is not an omission: there is no clap_id to watch. A
	/// swallowed control's card is subscribed to the STATE road instead, which is what carries a row.
	SegmentedControl(device::Modulatable& control, WidgetRegistry& registry,
	                 std::span<const char* const> names, Notifier* notifier = nullptr);
	/// **OFF THE REGISTRY'S LIST BEFORE THE MEMORY GOES** — `Knob::~Knob`'s rule for the modulation
	/// list, applied to the parameter one. Found under ASan, where `~FrameWatchers`'s assert is live;
	/// `NDEBUG` makes it compile to nothing in the ordinary build, so `ctest` was green throughout.
	///
	/// `WidgetRegistry::unwatch` sweeps every parameter rather than taking an id — a frame being
	/// destroyed does not remember what it subscribed to — so this is correct even for the constructor
	/// overload that never subscribed.


	void draw(visage::Canvas& canvas) override;

	/// Some choices only mean something in some modes — note priority does nothing in Poly, and the
	/// mock greys it out there rather than hiding it. A disabled control still draws its state; what
	/// it stops doing is answering the mouse.
	void setEnabled(bool enabled);
	bool enabled() const noexcept { return enabled_; }
	/// ENABLED HERE AND IN ITS PLACE: a control that answers `unavailable()` (a Poly switch inside a Rack) greys too.
	[[nodiscard]] bool usable() const { return enabled_ && (control_ == nullptr || control_->unavailable() == nullptr); }

	/// Switched out of the signal rather than out of bounds: draws in the disabled colours and goes on
	/// answering the mouse. The filter card's keytrack switch while the device is off — see
	/// Knob::setActive, which carries the reasoning for both.
	void setActive(bool active);
	bool active() const noexcept { return active_; }

	/// Stack the options instead of lining them up. The mock does this in the Engine rail
	/// (`.insp-sec .seg { flex-direction: column }`) while leaving the header's mode switch in a row,
	/// so it is a property of the instance rather than of the class.
	void setVertical(bool vertical);
	bool vertical() const noexcept { return vertical_; }

	/// Which segment a point falls in, or -1. Public so a test can ask rather than aim.
	int segmentAt(visage::Point position) const;

	/// **EVERY LABEL FITS ITS SEGMENT** at the size this control was given: each name's tracked width, as `draw` measures
	/// it, inside its cell less the border. A label that does not fit is drawn past its cell and read as two words run
	/// together ("eguliPoissCluster"), and CLAUDE.md's rule says a choice that does not fit the face goes to a menu.
	[[nodiscard]] bool labelsFit() const;

	/// Choose an option by index, as clicking it would.
	void select(int index);

	/// What this control addresses, whichever road built it. Public for the tests, which is the same
	/// reason Knob::control() is.
	device::Modulatable* control() const noexcept { return control_; }

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	/// The mouse handler both constructors install. See its definition for why this is a function
	/// rather than one constructor delegating to the other.
	void buildMouse();

	int selectedIndex() const;

	/// OWNED ONLY ON THE PARAMETER ROAD. The other constructor leaves this null and points control_ at
	/// somebody else's control — a device's, which outlives the widget. Knob's pair says the same.
	std::unique_ptr<ParamControl> owned_;
	device::Modulatable* control_ = nullptr;
	std::span<const char* const> names_;
	Notifier* notifier_ = nullptr;
	bool vertical_ = false;
	bool enabled_ = true;
	bool active_ = true;
};

/// A value with a minus and a plus either side of it — the mock's `.vcount`. For the parameters that
/// are counts and offsets rather than continuous quantities, where a slider would be a worse way to
/// ask for "one more semitone".
class Stepper : public visage::Frame, public TooltipSource {
public:
	/// The parameter's declared tooltip (its name when it declares none), anywhere on the row — `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipText(desc_.tooltip, desc_.name); }
	/// A CORE parameter, not a device control — it offers nothing a device's card or window is asked about.
	void offeredControls(std::vector<Offered>&) const override {}
	/// `step` is in the parameter's own units. Every parameter this is used for is stepped, so one is
	/// almost always right; BendRange is an index into a list and steps by one entry.
	Stepper(ParamBridge& bridge, WidgetRegistry& registry, const ParamDesc& desc, double step = 1.0,
	        Notifier* notifier = nullptr);
	/// **OFF THE REGISTRY'S LIST BEFORE THE MEMORY GOES** — `Knob::~Knob`'s rule for the modulation
	/// list, applied to the parameter one. Found under ASan, where `~FrameWatchers`'s assert is live;
	/// `NDEBUG` makes it compile to nothing in the ordinary build, so `ctest` was green throughout.
	///
	/// `WidgetRegistry::unwatch` sweeps every parameter rather than taking an id — a frame being
	/// destroyed does not remember what it subscribed to — so this is correct even for the constructor
	/// overload that never subscribed.


	void draw(visage::Canvas& canvas) override;

	/// A word in front of the reading, from the panel rather than from the parameter. The mock's rows
	/// read "bend ±2" and "8 voices" — the noun belongs to the row, not to the value, and it must not
	/// come from valueToText, which is the text a HOST shows and is not ours to decorate.
	void setRowLabel(const char* label);

	void setEnabled(bool enabled);
	bool enabled() const noexcept { return enabled_; }

	/// **THE HIGHEST VALUE THAT STILL CHANGES THE SOUND**, when something else caps the parameter — Voice Count
	/// under the stack's CPU guard (backlog KE). Values above it are the same sound, so the plus stops there (the
	/// honesty rule: a control is enabled iff it changes sound), and a stored value above it reads "16 (plays 5)", the
	/// setting and what it is held to. Infinity, the default, is no ceiling.
	void setCeiling(double ceiling);
	double ceiling() const noexcept { return ceiling_; }

	/// The words drawn between the buttons: the row label, the value, and the ceiling when it holds the value.
	std::string reading() const;

	/// -1 for the minus button, +1 for the plus, 0 for the label between them.
	int buttonAt(visage::Point position) const;

	/// Move by one step in either direction, clamped by the parameter's own range.
	void nudge(int direction);

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	clap_id id() const noexcept { return static_cast<clap_id>(desc_.id); }

	ParamBridge& bridge_;
	const ParamDesc& desc_;
	double step_;
	Notifier* notifier_ = nullptr;
	const char* rowLabel_ = nullptr;
	bool enabled_ = true;
	double ceiling_ = std::numeric_limits<double>::infinity();
};

} // namespace feathers::ui
