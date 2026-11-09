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
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/resonate_meter.h"
#include "plugin/ui/resonate_model_picker.h"
#include "plugin/ui/resonate_ring_view.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

// RESONATE'S CONTROLS AND ITS PICTURE — the card's few and the panel's all (Task 8; after the first listen,
// 2026-09-26, the card was far too big and was greatly simplified).
//
// **TWO SURFACES, TWO SETS OF WIDGETS, ONE PLACE THAT BUILDS THEM.** A face is built FOR a surface and seats
// only what that surface shows, so a card holds no hidden panel widgets to be reached by accident:
//
//   THE CARD — the string view (the Character x Morph drag surface, with its two boxes), the five model
//     pictograms, and six knobs in one row: Drive, Size, Pluck, Blend, Sustain, Mix. Nothing else.
//   THE PANEL — everything, by signal flow, left to right:
//     THE STACKS (left, v21/v22) — NOTES: Pluck, Jawari, Curve, Bounce, Sympathy; DRONE: Pluck, Jawari, Thread, Bounce,
//       Sympathy; Bridge Fixed under them.
//     THE STRING — the pictograms and the view, large; under it EXCITE (v22): the input meter and its pluck strip, Drive, Bow.
//     TONE — Poly|Mono, the Note Release chip (Gate / Ring) and Porta beside it (Mono only); Octave, Semitone,
//       Fine, Sustain.
//     DRONE — two rows: the Chord over its Tuning chip, then Excite, Root, Attack, Decay, Width; the Character and
//       Cycle zone cells, the Cycle's Rate, then Voice, Stiffness, Body, Twin. While Drone Out is 0 they change
//       nothing, so they are drawn disabled — the device's gate cannot say "level above 0" (it is a mask over
//       stepped values), and a custom surface can read the level and say it. The Cycle's pattern greys at Rate 0.
//     FILTER — Size, Width.  OUT — Blend, Level, Mix.
//
// WHAT REACHES WHAT, because the reachability guard takes the surfaces' word (`addressedParams`): every
// control from the panel; the card's eleven from the card; Enabled from the card's head.

namespace feathers::ui {

class ResonateFace {
public:
	enum class Surface { Card, Panel };

	ResonateFace(Surface surface, backplane::DeviceHost& host, DeviceInstanceId id, WidgetRegistry& registry,
	             Notifier* notifier, visage::Frame& parent);

	[[nodiscard]] Surface surface() const noexcept { return surface_; }
	[[nodiscard]] DeviceControl& control(fx::resonate::Param p) const;
	void setModulation(ModState* mod, EditHistory* history);

	/// **IS `p` LIVE RIGHT NOW** — Porta in Mono (its gate), and Root, Drone Attack and Drone Decay while the
	/// drone's level is above 0 (read here, since no gate can say it). Every other control always.
	[[nodiscard]] bool isLive(fx::resonate::Param p) const;

	//--------//
	// Layout //
	//--------//

	/// THE CARD'S KNOBS: smaller than the Engine rail's, three abreast in two rows.
	static constexpr float kCardKnobDiameter = 28.0f;
	static constexpr float kCardKnobHeight = Knob::columnHeight(kCardKnobDiameter);
	static constexpr float kCardKnobRows = 2.0f;
	static constexpr float kPanelKnobDiameter = Knob::kDiameter;
	static constexpr float kPanelKnobHeight = Knob::columnHeight(kPanelKnobDiameter);
	static constexpr float kCaptionHeight = 12.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kBoxGap = 4.0f;
	static constexpr float kMeterWidth = 22.0f;
	static constexpr float kSegmentHeight = 18.0f;
	static constexpr float kPickerHeight = 22.0f;

	/// THE CARD'S COLUMN, from `top` down, `width` wide from `left`. Returns the bottom of what it placed.
	float layOutCard(float left, float top, float width, bool visible);
	/// **THE CARD'S CONTENT HEIGHT FOR `width`** — the arithmetic `layOutCard` walks, without placing anything.
	[[nodiscard]] static float cardHeight(float width);
	/// THE STRING VIEW'S HEIGHT ON A CARD `width` wide.
	[[nodiscard]] static float cardRingHeight(float width);
	/// THE PANEL'S ROW, LEFT TO RIGHT across `area`.
	void layOutPanel(visage::Bounds area);
	/// THE SMALLEST AREA `layOutPanel` HOLDS WITHOUT OVERLAP.
	[[nodiscard]] static visage::Point panelMinimum();

	/// A SECTION HEADING AND WHERE IT IS DRAWN — the surface draws them with `drawCaptions`.
	struct Caption {
		const char* text;
		visage::Bounds box;
	};
	[[nodiscard]] const std::vector<Caption>& captions() const noexcept { return captions_; }
	void drawCaptions(visage::Canvas& canvas) const;

	//-------------//
	// Refreshing  //
	//-------------//

	/// WHAT CHANGED SINCE THE LAST CALL: the gates (a re-layout), the picture, the meter.
	struct Changes {
		bool gates = false;
		bool picture = false;
		bool meter = false;
		[[nodiscard]] bool any() const noexcept { return gates || picture || meter; }
	};
	Changes refresh();

	//-----------//
	// For tests //
	//-----------//

	/// EVERY CONTROL THIS SURFACE SEATS A WIDGET FOR.
	[[nodiscard]] std::vector<fx::resonate::Param> addressedParams() const;
	/// THE WIDGET ON `p`, or null when this surface seats none.
	[[nodiscard]] visage::Frame* widgetFor(fx::resonate::Param p) const;
	[[nodiscard]] ResonateRingView* ringView() const { return ring_.get(); }
	[[nodiscard]] ResonateModelPicker* modelPicker() const { return picker_.get(); }
	/// THE INPUT METER — the panel's; null on the card.
	[[nodiscard]] ResonateMeter* meter() const { return meter_.get(); }

private:
	struct KnobSlot {
		fx::resonate::Param param;
		std::unique_ptr<Knob> knob;
	};
	struct BoxSlot {
		fx::resonate::Param param;
		std::unique_ptr<ValueBox> box;
	};
	[[nodiscard]] Knob& knob(fx::resonate::Param p) const;
	[[nodiscard]] ValueBox& box(fx::resonate::Param p) const;
	void addKnob(fx::resonate::Param p, const char* caption, float diameter, Notifier* notifier, visage::Frame& parent);
	void addBox(fx::resonate::Param p, const char* caption, visage::Frame& parent);
	void caption(const char* text, float x, float y, float width);
	/// CHARACTER AND MORPH'S BOXES, OVER THE RING VIEW'S BANDS.
	void placeAxisBoxes(bool visible);
	/// THE DRONE'S THREE DEPENDANTS ENABLED EXACTLY WHILE ITS LEVEL IS ABOVE 0 — the panel's honesty.
	void applyDroneEnable();

	Surface surface_;
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::resonate::Param::kCount)> controls_ {};
	std::vector<KnobSlot> knobs_;
	std::vector<BoxSlot> boxes_;
	std::unique_ptr<SegmentedControl> voicing_;
	std::unique_ptr<ResonateModelPicker> picker_;
	std::unique_ptr<ResonateRingView> ring_;
	std::unique_ptr<ResonateMeter> meter_;
	/// CHARACTER AND MORPH, SEATED AS BOXES IN THE RING VIEW'S BANDS — their modulation road (see the view).
	std::unique_ptr<ValueBox> characterBox_;
	std::unique_ptr<ValueBox> morphBox_;
	/// THE PANEL'S DRONE ZONE CELLS AND ITS TWO CHIPS (Note Release, Tuning).
	std::unique_ptr<ZoneCoordCell> droneCharacter_;
	std::unique_ptr<ZoneCoordCell> cycle_;
	std::unique_ptr<SwitchChip> ringChip_;
	std::unique_ptr<SwitchChip> tuningChip_;
	std::unique_ptr<SwitchChip> transportChip_;
	std::unique_ptr<SwitchChip> followChip_;
	std::vector<Caption> captions_;

	int drawnVoicing_ = -1;
	int drawnModel_ = -1;
	int drawnDroneOn_ = -1;
	int drawnBridgeOn_ = -1;
	int drawnStringOn_ = -1;
	int drawnTwinOn_ = -1;
	int drawnCycleOn_ = -1;
	int drawnDriveOn_ = -1; // Drone Drive above 0: Drone Excite is live (v20)
	int drawnDroneBridgeOn_ = -1; // Drone Jawari above 0: Drone Bounce is live (v21)
	int drawnVoiceOn_ = -1; // Drone Voice above 0: Follow is live (v21)
};

} // namespace feathers::ui
