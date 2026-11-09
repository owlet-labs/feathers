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

#include "plugin/ui/widget_registry.h"
#include "fx/harm/harm_types.h"
#include "plugin/ui/controls.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/harm_scope.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <span>
#include <memory>
#include <string>
#include <vector>

// HARM'S EXPANDED VIEW — the picture, and the controls the card does not have room for.
//
// All controls, and a filter-style filter+harmonic visualiser, in the expanded view; only Level,
// Harmonic, Attack and Release on the card.
//
// TWELVE OF TWELVE. The expanded view carries every control — it is self-contained, and
// self-contained means self-contained: a surface that cannot switch its
// own device off is the kind of nearly-complete that ships and then gets explained.
//
// `Enabled` WAS LEFT OUT AND THE REASONING WAS SOUND FOR THE SURFACE IT WAS ABOUT — "the head has
// it" is true of a card and false of a panel that stands alone. It was then drawn here, in this body's
// modal row; since 2026-09-26 it is the WINDOW HEADER'S chip, one for every device (devices had been
// inconsistently adding their own active chips in the window) — still the declared control, so
// the window's chip and the card's square are one control addressed one way.
//
// AND IT IS NOT A KNOB LIST: the expanded view should be more intuitive than one. A list is
// UNDIFFERENTIATED, and these twelve are four different
// kinds of thing — which the old layout drew identically:
//
//   MODAL      Enabled, Poly            verbs, not values
//   (and Depth Follows Env, a switch in the temporal group: it hands the notch the envelope)
//   SPECTRAL   Harmonic, Fine, Level, Width, Depth    all five are IN the picture already
//   SPATIAL    Phase, Spread            the scope is mono and cannot show them
//   TEMPORAL   Attack, Release, Porta   the scope has no time axis at all
//
// So the groups are declared, labelled and laid out separately. **That grouping is the whole of the
// design pass in this change**; making the scope itself draggable for the spectral five is the next
// step and is deliberately not smuggled in here.
//
// THE REACHABILITY INVARIANT IS WHAT KEEPS THE COUNT HONEST rather than this comment: every control
// HARM declares must be offered by at least one surface, and the case sweeps `kParamDescs` rather
// than a list anybody maintains.
//
// IT TAKES ITS DATA FROM THE BRIDGE AND ITS SIZE FROM ITSELF. `ExpandedOverlay`'s properties 1 and 3:
// there is no parent in here, and the only size fact stated is a minimum — because the overlay this
// sits in today becomes a detached window later, and a window is resizable.

namespace feathers::ui {

class HarmExpanded final : public ExpandedView {
public:
	HarmExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	             Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	HarmExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	visage::Point minimumSize() const override;
	/// Backlog EI: WIDE WITHOUT LIMIT, since the harmonic row and the scope take the width; the height
	/// is capped at the natural size plus one `kPad`, since every row is a fixed height.
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kGroupGap = 16.0f;
	static constexpr float kCaptionHeight = 14.0f;
	/// THE SCOPE IS THE REASON THE PANEL EXISTS and it does not shrink to make room for controls —
	/// the brief says so outright. The overlay is a layer over the whole editor and is not short of
	/// space.
	static constexpr float kScopeHeight = 132.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// FOUR CELLS, WHICH IS THE WIDEST GROUP. Spectral has four knobs beneath its row, spatial two,
	/// temporal three — so a four-cell grid holds every group and the short ones leave cells empty
	/// rather than re-spacing. A row that re-spaced itself would move controls under the pointer for
	/// no reason a player can see; the card records the same rule.
	static constexpr int kRowCells = 4;
	static constexpr float kCellWidth = 62.0f;
	static constexpr float kSwitchHeight = 20.0f;
	static constexpr float kSwitchWidth = 84.0f;

	/// ONE GROUP OF KNOBS, DECLARED. The constructor builds from these, the layout walks them, and
	/// `minimumSize` counts them — so a control moving between groups is one line and the height
	/// follows it.
	struct KnobEntry {
		fx::harm::Param param;
		const char* label;
	};
	struct KnobGroup {
		const char* caption;
		std::span<const KnobEntry> knobs;
	};

	/// The groups, in the order they are drawn. See the header note for why these four.
	static std::span<const KnobGroup> groups();

	visage::Bounds scopeBounds() const;
	visage::Bounds harmonicBounds() const;
	visage::Bounds polyBounds() const;

	/// Where each group's caption is drawn, in declaration order. Filled by `resized`, read by
	/// `draw` — one geometry, so a case reading it reads what the panel writes.
	const std::vector<visage::Bounds>& captionBoundsForTest() const { return captionBounds_; }

	/// The bottom of the modal row, which is where the groups start.
	float modalBottom() const;

	/// The shared harmonic control — the same widget the card builds, over the same DeviceControl.
	HarmonicRow* harmonicRow() const { return row_.get(); }

	/// WHERE THE SWITCH'S CAPTION GOES, beside it. Not decoration: without it the control is the
	/// words "Off" and "On" with no subject, which is the defect `filter_card.cpp` names in as many
	/// words and which this panel shipped with for exactly one render.
	visage::Bounds polyLabelBounds() const;
	/// THE DEPTH-FOLLOWS-ENV SWITCH, in the TEMPORAL group's fourth cell — the one Attack, Release and Porta
	/// leave empty. It sits beside the two knobs that shape the envelope it hands the notch, because the
	/// envelope is what it is about. Filled by `resized`.
	visage::Bounds depthEnvBounds() const { return depthEnvBounds_; }
	/// Its caption, above it in the same cell.
	visage::Bounds depthEnvLabelBounds() const;
	SegmentedControl* depthEnvForTest() const { return depthEnv_.get(); }


	/// What that caption says — the word, or the word AND the reason while the switch is dead. The
	/// card's own text, asked of the same declaration, so the two surfaces cannot describe one
	/// control differently.
	std::string polyText() const;

	//-----------//
	// For tests //
	//-----------//

	/// EVERY CONTROL THIS SURFACE PUTS A HAND ON — the same question `HarmCard` answers, so the
	/// reachability invariant can take a union over the two without either knowing about the other.
	std::vector<fx::harm::Param> addressedParamsForTest() const;

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	const HarmScope* scopeForTest() const { return scope_.get(); }

	/// The Poly switch, which used to be `HarmCard::polyForTest`. THE CASES THAT ASKED THE CARD ASK
	/// THIS NOW rather than having been deleted — a control that moved is not a control that stopped
	/// mattering, and a suite that dropped its assertions when the split landed would have paid for
	/// the split by giving up the guarantees it was meant to preserve.
	SegmentedControl* polyForTest() const { return poly_.get(); }

	/// THE NOTCH / HIGH-PASS SWITCH (backlog NL), at the left of the modal row — the other verb that decides what
	/// the device IS rather than how much of it. The card has the same chip; the window is its superset.
	visage::Bounds modeBounds() const;
	SwitchChip* modeForTest() const { return mode_.get(); }
	static constexpr float kModeChipWidth = 76.0f;

	/// FOLLOWS THE MODE: in High-pass, Width, Depth and Env Depth shape nothing (the brickwall has one shape), so
	/// they grey — the honesty rule — and the scope drops their grips. Read off the host's field, like the card.
	bool updateContent() override;

private:
	DeviceControl& control(fx::harm::Param param) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::harm::Param> knobParams_;
	std::unique_ptr<SegmentedControl> poly_;
	std::unique_ptr<SegmentedControl> depthEnv_;
	std::unique_ptr<SwitchChip> mode_;
	int drawnMode_ = -1; ///< the Mode the greying last followed; -1 before the first
	void followMode(bool highpass);
	visage::Bounds depthEnvBounds_ {};
	std::unique_ptr<HarmScope> scope_;
	std::unique_ptr<HarmonicRow> row_;
	/// Where each group's caption is drawn, filled by `resized` so `draw` and the cases agree.
	std::vector<visage::Bounds> captionBounds_;
};

} // namespace feathers::ui
