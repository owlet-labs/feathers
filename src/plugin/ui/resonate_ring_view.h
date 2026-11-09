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

#include "backplane/device_host.h"
#include "device/modulatable.h"
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <string>
#include <vector>

// RESONATE'S STRING, DRAWN LIVE — the picture at the heart of its card and its panel (Task 8).
//
// **IT DRAWS WHAT THE DEVICE PUBLISHES AND NOTHING IT INVENTS.** One `fx::resonate::Reading` a tick, read
// through the host's readings (`DeviceHost::readingAt`), decoded by the device's own `unpackReading`. Per
// model: Ring and Stiffening as a closed loop, displacement RADIAL from a base circle; Bar as a line with
// free ends; Tube as a line pinned at both ends; Membrane as an 8x8 height map (row-major, row 0 at the
// top) with the 32-point scan circle over it. Always: the rest pose as a faint ghost under the live shape,
// and a flash where the last pluck landed, fading with its age.
//
// **THE HEADS ARE DRAWN BY WHAT A PICTURE CAN SHOW.** A head reads the whole path once per pitch cycle, so
// a dot at its phase is audio-rate strobe, not information — and with the Drone on it would never let the
// view rest. So there is ONE scan-origin marker, at the TRAVEL phase (the slow rotation of the ring under
// the heads, which moves at a readable rate), and the sounding heads as note chips along the bottom edge.
//
// **AND IT IS THE CHARACTER x MORPH SURFACE.** A drag anywhere on the plot moves Character across and Morph
// up, BY the hand rather than to it; double-click resets both. The puck is drawn at the MODULATED pair,
// with the base pair as a faint ghost when they differ. The two controls are also seated as value boxes in
// the view's bands (the face places them over `characterBand()` and `morphBand()`), which is where their
// modulation road — arm glow, drop, route menu — lives; this view is a picture and a drag, not a mod target.
//
// **ZERO REPAINTS AT REST** (Explore's rule). `refresh()` redraws only when something that is DRAWN moved:
// the shape, the ghost, the travel marker, the chips, a live flash, or the puck.

namespace feathers::ui {

class ResonateRingView final : public visage::Frame, public TooltipSource {
public:
	ResonateRingView(const backplane::DeviceHost& host, DeviceInstanceId id, device::Modulatable& character,
	                 device::Modulatable& morph);
	ResonateRingView(const backplane::DeviceHost&, size_t, device::Modulatable&, device::Modulatable&) = delete;

	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] const char* tooltipAt(visage::Point local) const override;
	/// A drag writes Character across and Morph up: a side road for each.
	void offeredControls(std::vector<Offered>& out) const override {
		out.push_back({ &character_, Road::Gesture });
		out.push_back({ &morph_, Road::Gesture });
	}

	/// **READ THE DEVICE AND REDRAW IF ANYTHING ON SCREEN WOULD CHANGE.** True when it redrew.
	bool refresh();

	//-----------//
	// Geometry  //
	//-----------//

	static constexpr float kInset = 6.0f;
	/// THE BANDS ALONG THE TOP AND THE BOTTOM: Morph's box and the model's name on top, Character's box and
	/// the head chips below — kept out of the plot, so neither the puck nor a cell ever sits under them.
	static constexpr float kBand = 17.0f;
	/// THE DISPLACEMENT FLOOR the drawn shape is normalised against, so a nearly still string is drawn
	/// nearly still rather than blown up to full swing.
	static constexpr float kScaleFloor = 0.05f;
	/// HOW MANY TICKS A PLUCK FLASH LASTS — about a quarter of a second at the string's ~344 Hz.
	static constexpr float kFlashTicks = 96.0f;
	static constexpr float kMarkerDiameter = 8.0f;
	static constexpr float kPuckDiameter = 11.0f;
	/// A CHIP'S WIDTH: kChipWidth while they fit, narrowing to kChipMinWidth (a three-character note name at the
	/// label size) before the chips take a second row (chips()).
	static constexpr float kChipWidth = 26.0f;
	static constexpr float kChipMinWidth = 18.0f;
	static constexpr float kChipGap = 3.0f;
	/// THE SMALLEST MEMBRANE CELL THAT CARRIES A REST RING — below it the ghost is noise, not information.
	static constexpr float kRestRingMinCell = 24.0f;

	/// WHERE THE PICTURE IS DRAWN — inside the inset, between the two bands.
	[[nodiscard]] visage::Bounds plotBounds() const;
	/// THE TOP BAND'S LEFT PART, where Morph's box sits, and the bottom band's, where Character's does.
	[[nodiscard]] visage::Bounds morphBand() const;
	[[nodiscard]] visage::Bounds characterBand() const;
	/// THE MODEL BEING DRAWN, from the reading (Ring when nothing is published).
	[[nodiscard]] fx::resonate::Model model() const;
	/// THE LIVE SHAPE'S POINTS, in drawing order — 32 for the chain models (Tube adds its two pins), and
	/// for Membrane the 32 scan-circle samples. Drawn from `shownNodes()`, the motion at display gain.
	[[nodiscard]] std::vector<visage::Point> livePath() const { return pathOf(shown_); }
	/// THE REST GHOST, unscaled — the pose the motion is measured from.
	[[nodiscard]] std::vector<visage::Point> restPath() const { return pathOf(reading_.rest); }
	/// WHERE A POINT `pos` ALONG THE SCAN PATH IS DRAWN, on the live shape.
	[[nodiscard]] visage::Point scanPoint(float pos) const { return pathPointAt(shown_, pos); }
	/// **A DISPLAY GAIN ON THE MOTION, LIKE A SCOPE'S — NOT THE STRING'S AMPLITUDE.** The live shape is drawn
	/// as `rest + kMotionGain * (nodes - rest)`: with real playing the string moves a few percent of its rest
	/// pose (MEASURED on a plucked chord: max |nodes - rest| ~ 0.07 against a rest pose of order 1), so at one
	/// to one the live loop lies on its ghost and the motion cannot be seen at card size. The rest ghost is
	/// drawn unscaled, and the shared ruler (`displayScale`) includes the amplified shape, so it never leaves
	/// the plot. Presentation only: nothing here reaches the sound.
	static constexpr float kMotionGain = 4.0f;
	/// THE SHAPE AS DRAWN: rest + kMotionGain * (nodes - rest), per node.
	[[nodiscard]] const std::array<float, fx::resonate::kPictureNodes>& shownNodes() const noexcept { return shown_; }
	/// THE RULER THE DRAWING USES: 1 / the largest |shown| or |rest| (floored at `kScaleFloor`).
	[[nodiscard]] float displayScale() const { return scale(); }
	/// **PARKED: the device's rest logic has frozen the string** (`Reading::parked`). It is drawn dimmed, with
	/// a HELD mark, so a still picture reads as a string held still rather than a picture that stopped.
	[[nodiscard]] bool parked() const noexcept { return hasReading() && reading_.parked >= 0.5f; }
	/// THE LIVE SHAPE'S ALPHA — 1 awake, `kParkedAlpha` parked.
	[[nodiscard]] float liveAlpha() const noexcept { return parked() ? kParkedAlpha : 1.0f; }
	static constexpr float kParkedAlpha = 0.4f;
	static constexpr const char* kParkedMark = "HELD";
	/// THE SCAN-ORIGIN MARKER: the travel phase, on the live shape.
	[[nodiscard]] visage::Point travelPoint() const { return scanPoint(reading_.travel); }
	/// MEMBRANE CELL `node` (row-major) — its drawn rectangle.
	[[nodiscard]] visage::Bounds membraneCell(size_t node) const;
	/// THE HEAD CHIPS, in drawing order: one per sounding note head (`headNotes`/`headGains`), then one per
	/// sounding drone string (`droneNotes`/`droneGains`, the chord's order) — its note, alpha, and which kind. EVERY
	/// sounding head has one: they narrow to kChipMinWidth, then take more rows upward from the bottom band.
	/// A DRONE CHIP IS QUIETER: an outline, where a played note's is filled, so the chord under the notes reads
	/// as the accompaniment it is.
	struct Chip {
		float note = 0.0f;
		float alpha = 0.0f;
		visage::Bounds box {};
		bool drone = false;
	};
	[[nodiscard]] std::vector<Chip> chips() const;
	/// HOW MANY ROWS THE CHIPS TAKE: 1 while every sounding head fits beside Character's box at kChipMinWidth; each
	/// row past the first is taken from the plot's bottom edge (plotBounds).
	[[nodiscard]] size_t chipRows() const;
	/// WHERE THE PUCK IS DRAWN: the MODULATED Character across, Morph up. `base` asks for the ghost.
	[[nodiscard]] visage::Point puckPoint(bool base = false) const;
	/// THE PLUCK FLASH'S ALPHA, 0 once it has faded.
	[[nodiscard]] float flashAlpha() const;
	/// WHETHER ANYTHING IS PUBLISHED AT ALL — without it the view says so rather than drawing a still string.
	[[nodiscard]] bool hasReading() const noexcept { return published_ > 0; }
	[[nodiscard]] const fx::resonate::Reading& reading() const noexcept { return reading_; }

	/// THE LINE THE PLOT SPEAKS, which names both of the controls it drags.
	static constexpr const char* kPlotTooltip = "Drag: Character across, Morph up";
	/// AND A NOTE CHIP'S, followed by the note name.
	static constexpr const char* kChipTooltipPrefix = "A head sounding ";
	/// AND A DRONE CHIP'S.
	static constexpr const char* kDroneChipTooltipPrefix = "A drone string sounding ";

private:
	/// THE CHIPS' WIDTH TO THE RIGHT OF CHARACTER'S BOX.
	[[nodiscard]] float chipRoom() const;
	/// EVERYTHING THE PICTURE DEPENDS ON, and nothing it does not — `heads` (audio-rate) and `energy`
	/// (undrawn) are not in it, and a pluck's age is in it only as the flash it draws.
	struct Drawn {
		fx::resonate::Reading reading {};
		size_t published = 0;
		float flash = 0.0f;
		double character = 0.0;
		double morph = 0.0;
		double characterBase = 0.0;
		double morphBase = 0.0;
		bool operator==(const Drawn&) const;
	};
	[[nodiscard]] Drawn snapshot() const;

	[[nodiscard]] visage::Point pathPointAt(const std::array<float, fx::resonate::kPictureNodes>& nodes,
	                                        float pos) const;
	[[nodiscard]] std::vector<visage::Point> pathOf(const std::array<float, fx::resonate::kPictureNodes>& nodes) const;
	/// THE DISPLACEMENT SCALE, shared by the live shape and the ghost so they are drawn to one ruler.
	[[nodiscard]] float scale() const;
	[[nodiscard]] float membraneSide() const;

	void drawMembrane(visage::Canvas& canvas, const visage::Bounds& box);
	void drawShape(visage::Canvas& canvas, const std::vector<visage::Point>& pts, bool closed, float thickness);

	const backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	device::Modulatable& character_;
	device::Modulatable& morph_;

	fx::resonate::Reading reading_ {};
	std::array<float, fx::resonate::kPictureNodes> shown_ {};
	size_t published_ = 0;
	Drawn drawn_ {};
	bool drawnOnce_ = false;
	/// THE CHIP TOOLTIP'S TEXT, held here because `tooltipAt` hands back a pointer.
	mutable std::string chipTip_;

	bool dragging_ = false;
	visage::Point anchor_ {};
	double anchorCharacter_ = 0.0;
	double anchorMorph_ = 0.0;
};

} // namespace feathers::ui
