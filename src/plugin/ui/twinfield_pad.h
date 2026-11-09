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
#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/field_activity_map.h"

#include "visage_ui/frame.h"

#include <array>
#include <cstddef>
#include <functional>

namespace feathers::ui {

/// **TWIN FIELD'S PAD: ONE LAYER'S TERRAIN, ITS EIGHT READING POINTS, AND ITS ORBIT.**
///
/// The terrain is AutoMod's pad's (the same `activity`, the same palette, the same scale: a half-view of
/// one zone at zoom 1), read through `twinfield::terrainAt`, the very function the device's outputs read.
/// So each dot sits on the brightness its output publishes at rest.
///
/// The view is centred on the layer's PAN point. The ring of dots is sized on the TERRAIN by Spread (2026-11-08,
/// decoupled from Zoom), so Zoom changes only the picture's scale and a zoomed-in pad draws the ring larger. A DRAG pans (writes Pan X/Y) with the puck following the hand, AutoMod's and
/// Explore's sign; Ctrl/Cmd+WHEEL zooms (writes Zoom), and a plain wheel is the lane's scroll.
class TwinFieldPad final : public visage::Frame, public TooltipSource {
public:
	/// One layer's controls the pad reads or writes, by `LayerParam`.
	using LayerControls = std::array<device::Modulatable*, mod::twinfield::kLayerParams>;

	explicit TwinFieldPad(std::array<LayerControls, mod::twinfield::kLayers> layers);

	void setLayer(size_t layer);
	[[nodiscard]] size_t layer() const noexcept { return layer_; }

	void draw(visage::Canvas& canvas) override;
	bool mouseWheel(const visage::MouseEvent& e) override;
	/// **THE PAD'S FOCUS — zoom does not take focus; it only zooms when focused.** A press on the pad focuses it (a
	/// subtle outline); while focused a PLAIN wheel zooms, and while not, every wheel but Ctrl/Cmd+wheel goes to the lane.
	/// The wheel never focuses. Focus ends on a click elsewhere (visage moves its keyboard focus: `focusChanged`), on
	/// Esc, or when the pad or its card is hidden (folded, or scrolled out of the lane). It is VISAGE's focus only,
	/// never the X focus (`keyboard_claim.h`): every key but Esc goes on up and out to the DAW.
	[[nodiscard]] bool focused() const noexcept { return focused_; }
	void blur();
	void focusChanged(bool isFocused, bool wasClicked) override;
	bool keyPress(const visage::KeyEvent& e) override;
	void visibilityChanged() override;
	void offeredControls(std::vector<Offered>& out) const override;
	/// THE PAN CONTROL'S OWN LINE ("... drag the pad to move it"): the drag is the pad's main gesture.
	const char* tooltipAt(visage::Point) const override;

	/// ONE WHEEL NOTCH, in octaves of zoom.
	static constexpr double kZoomStepOct = 0.25;
	static constexpr size_t kMapWide = 40;
	static constexpr size_t kMapHigh = 40;

	/// Plane units per pixel at the shown layer's zoom.
	[[nodiscard]] double planePerPixel() const;
	/// **WHERE THE ORBIT IS NOW** — the device publishes each layer's orbit phase (the playhead channel: A as the phase,
	/// B as the level), and the card hands the pad a reader for it. False: nothing published, and the puck is drawn at
	/// its pan point. Set by the owner; unset reads as nothing published.
	using PhaseReader = std::function<bool(size_t layer, double* phase)>;
	void setPhaseReader(PhaseReader reader) { phaseReader_ = std::move(reader); }
	[[nodiscard]] bool livePhase(double* phase) const;
	/// THE RING'S RADIUS ON THE GLASS, in pixels: Spread's terrain-unit radius through the zoom's plane-per-pixel.
	[[nodiscard]] float ringPixels() const;
	/// WHERE THE PUCK IS DRAWN, in this frame's pixels: the pan point (the centre) plus the orbit at the published phase.
	[[nodiscard]] visage::Point puckPixel() const;
	/// WHERE POINT k IS DRAWN, in this frame's pixels — around the puck, riding with it.
	[[nodiscard]] visage::Point pointPixel(size_t k) const;
	[[nodiscard]] const std::array<float, kMapWide * kMapHigh>& mapForTest() const noexcept { return map_.cells(); }
	[[nodiscard]] size_t rebuildsForTest() const noexcept { return rebuilds_; }

private:
	struct Key {
		double panX = -1e9, panY = -1e9, zoom = -1e9, rotate = -1e9, gamma = -1e9;
		float w = -1.0f, h = -1.0f;
		size_t layer = 99;
		bool operator==(const Key&) const = default;
	};
	[[nodiscard]] double field(mod::twinfield::LayerParam p) const;
	[[nodiscard]] Key keyNow() const;
	void rebuildIfMoved();

	std::array<LayerControls, mod::twinfield::kLayers> layers_;
	size_t layer_ = 0;
	PhaseReader phaseReader_;
	FieldActivityMap<kMapWide, kMapHigh> map_;
	Key built_ {};
	size_t rebuilds_ = 0;

	bool held_ = false;
	bool focused_ = false;
	visage::Point pressAt_ {};
	double pressPanX_ = 0.0;
	double pressPanY_ = 0.0;
};

} // namespace feathers::ui
