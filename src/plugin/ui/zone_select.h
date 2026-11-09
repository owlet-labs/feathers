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

#include <visage/app.h>

#include <string>
#include <vector>

// **THE ZONE SELECTOR — backlog BJ, piece 2 of 4.**
//
// The named zone regions move to a dropdown with prev/next, instead of being displayed on the terrain
// and controlled by the 1-D home position — and this satisfies the earlier decision that a named zone
// is displayed somewhere. Here is the somewhere.
//
// **IT IS A SELECTOR OVER ONE COORDINATE, NOT AN INPUT OF ITS OWN — option (A), the one chosen.**
// Picking a zone MOVES the coordinate into that zone; the coordinate remains the only thing any
// device consumes, so nothing sounds different on the day this lands. Option (B) — a zone id the
// engine reads alongside the coordinate — is a DSP change in `deriveCurve`, `deriveAutomod` and
// OWLTT's character selection, and was declined for now.
//
// **AND IT SHOWS NO NUMBER, WHICH IS THE HALF THAT MAKES THE COLLAPSE WORK.** The design is a zone
// dropdown and one value box — two widgets, and each says the thing the other cannot. The name
// is ambiguous on its own, because the names REPEAT: eight zones fill one lap of the line and the
// line is infinite, so "Width" names eight-plus regions rather than one. The `ValueBox` beside this
// carries the coordinate itself, lap included, and that is what disambiguates them. A selector that
// also printed a number would be the coarse:fine pair again wearing a dropdown.

namespace feathers::ui {

class ModState;

class ZoneSelect final : public visage::Frame, public TooltipSource {
public:
	/// The coordinate's declared tooltip, anywhere on the strip — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	/// **A DESTINATION, AS A DROPDOWN IS** (Pattern's controls did not glow): armed and routable, the
	/// selector glows and a press-drag (up) sets the armed source's amount instead of picking — a double-click unroutes;
	/// routed and not armed, a mark in the modulation colour says so. Alt picks whatever is armed. `ValueBox`'s road.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;
	/// Armed, routable, enabled, and Alt not held: a press would route rather than pick.
	[[nodiscard]] bool mappable(bool altDown) const;
	/// ZONE NAMES — what each named region is called. Declared beside the CONTROL's declaration in
	/// the device, never in a UI map, so a standalone device still carries them. Same struct shape as
	/// `ZoneCoordBox::ZoneName`, which this replaces; the names were always the good part of it.
	struct ZoneName {
		std::string name;        ///< e.g. "Width", "Punch", "Air"
		std::string description; ///< what this region does, for tooltips
	};

	/// **WHAT ONE FULL LAP OF THE NAMED SET IS WORTH, IN THE COORDINATE'S OWN UNITS.**
	///
	/// One, and it is not a new number: it is `ZoneCoordBox::kPhasePerLap`, the quantum the carry has
	/// always used. The offsets behind it are consumed as `wrapPhase(offset * kPhiNNN)` — a phi
	/// multiplier per field, wrapped — so 1 lands the fields on a different, unrepeating set of
	/// phases, which is what the firmware's encoder does when it wraps. A `static_assert` in the .cpp
	/// pins the two together so a change to one cannot silently leave the other behind.
	static constexpr double kCoordinatePerLap = 1.0;

	/// @param coordinate the single collapsed control — unbounded, and the only thing the DSP reads
	/// @param names      the named regions, in order, from the device
	/// @param perLap     how much coordinate one full pass through `names` is worth
	///
	/// **`device::Modulatable&` RATHER THAN `FxControl&`**, for the reason `ZoneCoordBox` takes one:
	/// a coordinate is not an fx-device idea. Explore's gamma is the same shape and its halves are a
	/// `ParamControl` and a bespoke adapter, neither of which is an `FxControl`.
	ZoneSelect(device::Modulatable& coordinate, std::vector<ZoneName> names,
	           double perLap = kCoordinatePerLap);

	void draw(visage::Canvas& canvas) override;

	//-- what it says -----------------------------------------------------------------------------//

	[[nodiscard]] size_t zoneCount() const noexcept { return names_.size(); }

	/// **WHICH CONTROL THIS SELECTOR IS OVER.** Exposed so a case can assert that a cell's selector
	/// and its value box address the SAME quantity — two widgets over one control is the whole shape
	/// of the collapse, and if they ever drift apart the caption describes somewhere the number is
	/// not. Identity is not recoverable from anything else either widget shows.
	[[nodiscard]] const device::Modulatable& control() const noexcept { return control_; }

	/// HOW MUCH COORDINATE ONE ZONE IS WORTH — `perLap / zoneCount`. One number derived from two
	/// rather than a third constant, so a device that changes its zone count cannot leave a stale
	/// width behind.
	[[nodiscard]] double perZone() const noexcept;

	/// WHICH NAMED REGION THE COORDINATE IS IN, always in `0 .. zoneCount()-1`.
	///
	/// **IT WRAPS, BECAUSE EVERY ZONED COORDINATE IN THIS PROJECT IS NOW A LINE.** `computeZone` in
	/// `device/zone_param.h` clamps to 0..1, which was right while the zoned halves were bounded and
	/// is wrong for a coordinate running to ±2²³: everything past the first lap would report the last
	/// zone forever. The bounded form ended on 2026-09-09 — none of these is bounded — so there is one
	/// rule here rather than two.
	///
	/// **A CLAMPING BRANCH EXISTED FOR ABOUT AN HOUR AND WAS DELETED WHEN IT BECAME UNREACHABLE.** It
	/// served OWLTT's `Character` while that was still 0..1; a sweep of every declared control confirms
	/// no zoned param is bounded any more, and an untested branch nothing reaches is worse than none.
	[[nodiscard]] size_t zoneIndex() const;


	/// WHICH PASS THROUGH THE NAMED SET WE ARE ON — 0 for the first, negative below the origin.
	///
	/// **THE NAMES REPEAT AND THIS IS THE NUMBER THAT SAYS SO.** It is deliberately NOT drawn here:
	/// the `ValueBox` beside this widget shows the coordinate, which carries the lap in its own
	/// digits. Exposed because a case asserting where a gesture landed needs the whole answer, and
	/// because `selectZone` is defined in terms of it.
	[[nodiscard]] long lap() const;

	/// WHERE INSIDE THE ZONE, 0..1. The part of the coordinate no zone gesture ever disturbs.
	[[nodiscard]] double positionInZone() const;

	/// THE DEVICE'S OWN NAME FOR A REGION, WHICH MAY BE EMPTY — and that is a real state rather than
	/// a gap in the data. A region the engine does nothing special in has nothing to be called.
	/// (OWLTT's character zones 1 and 2 were the standing example until 2026-09-16, when the parity
	/// sweep found the DSP had cases for both; they are Timing and Skew now, and the rule below stands
	/// on its own.)
	[[nodiscard]] std::string zoneName() const;
	[[nodiscard]] std::string nameOf(size_t index) const;

	/// **WHAT THE SELECTOR ACTUALLY SAYS** — the name where there is one, the region's NUMBER where
	/// there is not. `ZoneCoordBox::regionText` settled this and the rule carries: a caption that
	/// simply vanished for zones 1 and 2 would read as a broken widget, and worse, it would read
	/// IDENTICALLY in two different places, which is the ambiguity that whole widget's comment warns
	/// about.
	///
	/// Empty only when there are no names at all — Explore's gamma, a continuum with no vocabulary
	/// over it, where inventing region numbers would claim a structure the quantity does not have.
	[[nodiscard]] std::string captionText() const;
	[[nodiscard]] std::string captionOf(size_t index) const;

	//-- the verbs, which are public because a case cannot click -----------------------------------//
	//
	// X11 pointer synthesis is banned on the development display, so a gesture whose behaviour lives only in a
	// mouse handler is a gesture nothing can assert. `mod_route_menu.h` made the same split for the
	// same reason: the popup is a shell, and everything it DOES is a verb a case can call directly.

	/// **MOVE `delta` ZONES ALONG THE LINE.** Prev is -1, next is +1. It crosses laps freely — one
	/// step past the last zone is the first zone of the next lap, which is exactly the firmware's
	/// "just keep turning" past the 8th zone, which keeps driving the phase.
	void stepZone(int delta);

	/// **JUMP TO A NAMED REGION IN THE CURRENT LAP.**
	///
	/// **IT DOES NOT CROSS A LAP, AND THAT IS A CHOICE RATHER THAN AN OVERSIGHT.** Picking zone 0
	/// while standing in zone 7 could mean the zone 0 one step FORWARD (next lap) or the one seven
	/// steps back (this lap), and both are defensible. Same-lap wins because the lap is a separate
	/// quantity the user can see in the box beside this one: a jump that also moved it would be two
	/// edits from one gesture, and the number would visibly jump for a reason the caption does not
	/// explain. `stepZone` is the gesture that crosses laps, and it says so by moving one at a time.
	///
	/// THE POSITION WITHIN THE ZONE SURVIVES, because the move is always a whole multiple of
	/// `perZone`. Somebody who has dialled two-thirds of the way into "Punch" arrives two-thirds of
	/// the way into "Air".
	void selectZone(size_t index);

	//-- geometry and gestures ---------------------------------------------------------------------//

	enum class Verb {
		None, ///< not on the widget, or on it and meaning nothing
		Prev,
		Next,
		Menu, ///< the caption: open the list
	};

	/// WHAT A PRESS AT THIS POINT MEANS, without doing it. Asked by `pressAt` and by the case that
	/// checks every x across the widget answers exactly one verb — the same guard `FxCard`'s head
	/// carries, and for the same reason: two verbs sharing a pixel is a control that does the wrong
	/// thing occasionally rather than never.
	[[nodiscard]] Verb verbAt(visage::Point position) const;

	/// PERFORM THE VERB UNDER THE POINT. Returns whether one was taken, so a caller's press can fall
	/// through to its ordinary road when the answer is `None`.
	bool pressAt(visage::Point position);

	[[nodiscard]] visage::Bounds prevBounds() const;
	[[nodiscard]] visage::Bounds nextBounds() const;
	[[nodiscard]] visage::Bounds captionBounds() const;

	/// **THE NARROWEST THIS SELECTOR CAN BE AND STILL PRINT WHAT IT MAY PRINT** — two arrow cells at
	/// full width plus the widest caption IN THE SET, not the one showing now. A width taken from the
	/// current name is a width that fits until the coordinate moves, which is the sampling mistake
	/// `widestReadoutWidth` in `zone_coord_cell.cpp` already carries the scar of.
	///
	/// **IT MEASURES A FONT, so it may never be reached from a `Rail::expandedWidth()`** — see
	/// `rail.h`. `CircleRail` STATES its cell side as a constant and pins it against this in a case,
	/// which is the arrangement that gets a checked number without a measured width.
	[[nodiscard]] float minimumWidth() const;

	/// THE ARROW CELLS ARE SQUARE AND CAPPED, so the caption keeps the middle on a wide row and the
	/// arrows do not grow into it.
	static constexpr float kArrowWidth = 14.0f;
	static constexpr float kChevronHalf = 3.0f;
	static constexpr float kCornerRadius = 4.0f;

	/// GREYED WITH THE REST OF ITS CARD — a `Frame` has no such notion, so this carries the flag
	/// `Knob` and `ZoneCoordBox` both carry, for the reason they carry it: a device switched out of
	/// the signal reads as off, and that is a property of the CARD rather than of the control.
	void setEnabled(bool on);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
	/// HOW MANY ZONES FROM THE ORIGIN, signed and unwrapped — the one derivation `zoneIndex`, `lap`
	/// and `positionInZone` all read, so the three cannot disagree about where the boundaries are.
	[[nodiscard]] double zonesFromOrigin() const;
	/// BRACKETED, WHICH IS THE UNDO STEP. A bare `setValue` writes the field and forgets it; the
	/// history entry is what `beginGesture` records. Every zone gesture goes through here.
	void commit(double coordinate);

	device::Modulatable& control_;
	std::vector<ZoneName> names_;
	double perLap_ = kCoordinatePerLap;
	bool enabled_ = true;
	// THE ROUTE GESTURE (`mappable`): the press in flight sets an amount; the panel state is watched so the glow repaints
	// when the arm moves (`ValueBox`'s subscription).
	bool routing_ = false;
	float routeStartY_ = 0.0f;
	float anchorAmount_ = 0.0f;
	Watch watch_;
	const ModState* watched_ = nullptr;
	void refreshWatch();
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;
};

} // namespace feathers::ui
