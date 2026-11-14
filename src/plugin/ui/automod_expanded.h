/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/widget_registry.h"

#pragma once

#include "backplane/device_host.h"
#include "fx/automod/automod_types.h"
#include "plugin/ui/automod_pad.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/zone_coord_cell.h"
#include <span>
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// AUTOMOD'S WINDOW — the coordinates and the plane their phases live in.
//
// **WHY THIS DEVICE GETS ONE, IN ITS OWN TERMS.** The card carries what the device DOES — rate, sync,
// mode, depth, manual, mix — and had grown an eighth knob because there was nowhere else to put it.
// What it could never carry is WHERE THE DEVICE IS STANDING: three coordinates, each with a phase
// offset, plus a device-wide gamma. Four of those seven are phases, and a phase on a knob is a number
// nobody can read and nobody can aim.
//
// **THE PANEL'S CLAIM IS THAT THE THREE PHASE OFFSETS ARE A PLANE, AND THAT IS ARITHMETIC RATHER
// THAN LAYOUT.** `deriveAutomod` folds each coordinate as `coord + coordPhase + gammaPhase`, so gamma
// is the COMMON MODE — the term that moves all three together — and the three offsets are what is
// left. The part of three numbers a common term cannot express is exactly TWO-DIMENSIONAL. So the pad
// does not approximate three controls with two; it spans precisely the space gamma does not, and
// gamma keeps its own knob because the two are orthogonal by construction.
//
// SO THE DIVISION OF LABOUR IS: the CARD is how it plays, the PANEL is where it is standing. Nothing
// moved off the card to make room — moving `Env` there is filed separately, and that is a separate change
// with a height budget behind it.
//
// ROTATE IS ON THE PANEL AND NOT THE CARD because it means nothing without the pad. It re-aims the
// basis the pad projects onto, so on a surface with no pad it would be a knob that changes what an
// invisible thing points at.

namespace feathers::ui {

class AutomodExpanded final : public ExpandedView {
public:
	AutomodExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	AutomodExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	visage::Point minimumSize() const override;
	/// Backlog EI: CAPPED BOTH WAYS at the natural size plus one `kPad`. The pad is a fixed square and
	/// the rows fixed heights; the knob row spreads its cells over a wider window, which moves nothing
	/// into the room.
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kSwitchHeight = 20.0f;

	/// THE PAD IS SQUARE, unlike the shaper's, because its decode is polar — see `AutomodPad`.
	static constexpr float kPadSide = 300.0f;

	/// **THE CELL GRID. FOUR ROWS, AND THE WINDOW IS A SUPERSET OF THE CARD.**
	///
	/// BY DESIGN an expanded window is always a superset of the controls on the card. This window
	/// reached six of its card's controls and missed ten; the
	/// structural test in `test_gui_widgets.cpp` found the same fault on Drawn's window, which nobody
	/// had looked at.
	///
	/// THE ROWS ARE THE CARD'S READING, WIDENED. Row 1 is the coordinate row the card has — the three
	/// coarse:fine pairs and gamma, through the SAME seating (`automod_coords.h`), so the two surfaces
	/// show the same values by construction rather than by coincidence. Rows 2 and 3 are the card's
	/// performance grid at its own `{3, 4}` split, so a hand that knows the card finds them in the
	/// shape it already learned. Row 4 is what only the window has.
	static constexpr size_t kCoordCells = 4;
	static constexpr size_t kCellCount = 14;
	static constexpr float kCellWidth = 84.0f;

	/// HOW MANY CELLS IN EACH ROW, in order. Declared here so the constructor and `layOut` cannot
	/// disagree — the card's own note, and it caught a bug there.
	[[nodiscard]] static std::span<const size_t> cellRows();

	[[nodiscard]] visage::Bounds padBounds() const;

	/// EVERY CONTROL THIS PANEL'S BODY REACHES — the knobs it built, plus the pad's three phase offsets.
	/// (The enable is the window header's chip.) The reachability guard takes this at its word, so it is written from what the
	/// widgets actually address rather than from what the panel means to cover.
	[[nodiscard]] std::vector<fx::automod::Param> addressedParamsForTest() const;

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }
	[[nodiscard]] const AutomodPad* padForTest() const { return pad_.get(); }

	/// GAMMA'S BOX, so a case can assert the seating rather than photograph it.
	[[nodiscard]] const GammaBox* gammaForTest() const { return gamma_.get(); }

	/// ROTATE'S BOX — the widget that replaced its knob when its range became a drag window. A case
	/// asks this for the same reason it asks for gamma's: what a drag is worth is not visible from
	/// outside the widget, and "it is a box at all" is half the fix.
	[[nodiscard]] const GammaBox* rotateForTest() const { return rotate_.get(); }
	[[nodiscard]] const SwitchChip* keytrackForTest() const { return keytrack_.get(); }

	/// THE THREE COORDINATE BOXES, so a case can ask WHICH control each half holds — see the case that
	/// pins it. `owltt_expanded` exposes the same thing for the same reason.
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& coordsForTest() const {
		return coords_;
	}

	[[nodiscard]] fx::automod::Param paramOfKnobForTest(const Knob* knob) const {
		for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
			if (knobs_[i].get() == knob) {
				return knobParams_[i];
			}
		}
		return fx::automod::Param::kCount;
	}

private:
	[[nodiscard]] DeviceControl& control(fx::automod::Param p) const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::automod::Param> knobParams_;
	/// Sync and LFO Mode: named choices, so boxes that open their lists rather than knobs.
	std::vector<std::unique_ptr<ValueBox>> choices_;
	std::vector<fx::automod::Param> choiceParams_;
	std::vector<visage::Frame*> cells_;
	std::unique_ptr<GammaBox> gamma_;
	/// ROTATE'S BOX. Not a knob since its range became a drag window — see the seating in the .cpp.
	std::unique_ptr<GammaBox> rotate_;
	std::unique_ptr<SwitchChip> keytrack_;
	std::vector<std::unique_ptr<ZoneCoordCell>> coords_;
	std::vector<fx::automod::Param> coordParams_;
	std::unique_ptr<AutomodPad> pad_;
	/// The pad's drag, held as one undo step (backlog JB).
	DeviceFieldsGesture padGesture_;

	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
