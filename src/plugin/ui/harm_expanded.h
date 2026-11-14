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

#include "plugin/ui/controls.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/harm_envelope_view.h"
#include "plugin/ui/harm_partial_strip.h"
#include "plugin/ui/harm_scope.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <span>
#include <memory>
#include <string>
#include <vector>

// HARM'S EXPANDED VIEW — four partials and an envelope, composed around the picture.
//
// TOP TO BOTTOM:
//
//   PARTIALS    a strip of four columns — each partial's number and ratio, its Active switch and its Level — ALWAYS on
//               show, so every partial is switched and levelled from whichever tab is open;
//   TABS        "1 2 3 4", drawn as Drawn's frame tabs are (`tab_chip.h`): which partial the detail below is about;
//   DETAIL      left: the selected partial — its ratio row and Mode, the scope (every active partial's notch and mark, the
//               selected one emphasised), and Fine, Width, Depth, Phase, Spread. Right: the ENVELOPE, its curve and its six
//               values;
//   MASTER      Porta, Poly and Env Depth — the controls that belong to the device and to no one partial.
//
// THE TAB IS VIEW STATE, exactly as Drawn's selected frame is: a plain member of the window, not a parameter, not automated,
// not modulated, and not stored. All four partials' detail controls are built once and the selected set is shown, so the four
// tabs are one layout bound to four sets of parameters.
//
// THE SLOT'S MIX AND WET GAIN ARE NOT HERE: they are the site panel's furniture, supplied by the slot, and the decision that
// the window never draws them too is held by a case ("THE PANEL ALONE REACHES ALL …").
//
// EVERY CONTROL IS REACHABLE FROM THE WINDOW, which `addressedParamsForTest` lists and a case sweeps over the declarations.
//
// IT TAKES ITS DATA FROM THE BRIDGE AND ITS SIZE FROM ITSELF. `ExpandedOverlay`'s properties 1 and 3: there is no parent in here,
// and the only size fact stated is a minimum — because the overlay this sits in today becomes a detached window later, and a
// window is resizable.

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
	static constexpr float kGroupGap = 14.0f;
	static constexpr float kCaptionHeight = 14.0f;
	static constexpr float kCaptionGap = 6.0f;
	/// THE SCOPE IS THE REASON THE PANEL EXISTS and it does not shrink to make room for controls.
	static constexpr float kScopeHeight = 120.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kTabHeight = 22.0f;
	static constexpr float kSwitchHeight = 20.0f;
	/// THE DETAIL'S TWO COLUMNS: the selected partial on the left, the envelope on the right.
	static constexpr float kLeftWidth = 340.0f;
	static constexpr float kRightWidth = 250.0f;
	static constexpr float kColumnGap = 20.0f;
	static constexpr int kKnobCells = 5;
	static constexpr float kModeChipWidth = 76.0f;
	static constexpr float kPolyChipWidth = 64.0f;
	static constexpr float kEnvDepthChipWidth = 84.0f;
	static constexpr float kPortaWidth = 100.0f;

	/// WHICH PARTIAL THE DETAIL IS ABOUT, 0..3 — view state. Setting it shows that partial's controls and tells the scope.
	int selectedPartial() const noexcept { return selected_; }
	void selectPartial(int partial);

	/// THE CAPTIONS, in the order they are drawn, and where `resized` put them — one geometry, so a case reads what is drawn.
	struct Caption {
		const char* text;
		visage::Bounds bounds;
	};
	const std::vector<Caption>& captionsForTest() const { return captions_; }
	const std::vector<visage::Bounds> captionBoundsForTest() const {
		std::vector<visage::Bounds> out;
		for (const Caption& c : captions_) {
			out.push_back(c.bounds);
		}
		return out;
	}

	visage::Bounds stripBounds() const;
	visage::Bounds tabBounds(int partial) const;
	/// What a tab says: the partial's number.
	static std::string tabText(int partial);
	visage::Bounds harmonicBounds() const;
	visage::Bounds scopeBounds() const;
	visage::Bounds modeBounds() const;
	visage::Bounds knobRowBounds() const;
	visage::Bounds envelopeBounds() const;
	visage::Bounds envelopeBoxBounds(size_t index) const;
	visage::Bounds masterBounds() const;
	visage::Bounds portaBounds() const;
	visage::Bounds polyBounds() const;
	visage::Bounds depthEnvBounds() const;
	/// The bottom of the strip's caption and the strip itself, which is where the tabs begin.
	float modalBottom() const;

	/// The selected partial's harmonic control — the same widget the card builds, over the same DeviceControl family.
	HarmonicRow* harmonicRow() const { return detail_[static_cast<size_t>(selected_)].row.get(); }
	HarmonicRow* harmonicRow(int partial) const { return detail_[static_cast<size_t>(partial)].row.get(); }

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

	/// THE SELECTED PARTIAL'S KNOBS (Fine, Width, Depth, Phase, Spread) — those of the others are built and hidden.
	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return detail_[static_cast<size_t>(selected_)].knobs; }
	const std::vector<std::unique_ptr<Knob>>& knobsForTest(int partial) const { return detail_[static_cast<size_t>(partial)].knobs; }
	const HarmScope* scopeForTest() const { return scope_.get(); }
	HarmScope* scopeForTest() { return scope_.get(); }
	const HarmPartialStrip* stripForTest() const { return strip_.get(); }
	const HarmEnvelopeView* envelopeViewForTest() const { return envelopeView_.get(); }
	const std::vector<std::unique_ptr<ValueBox>>& envelopeBoxesForTest() const { return envelopeBoxes_; }
	ValueBox* portaForTest() const { return porta_.get(); }

	/// The Poly switch, which used to be `HarmCard::polyForTest`. THE CASES THAT ASKED THE CARD ASK
	/// THIS NOW rather than having been deleted.
	SwitchChip* polyForTest() const { return poly_.get(); }
	SwitchChip* depthEnvForTest() const { return depthEnv_.get(); }

	/// THE NOTCH / HIGH-PASS SWITCH (backlog NL) of the selected partial — the other verb that decides what
	/// that partial IS rather than how much of it. The card has the same chip for the first; the window is its superset.
	SwitchChip* modeForTest() const { return detail_[static_cast<size_t>(selected_)].mode.get(); }
	SwitchChip* modeForTest(int partial) const { return detail_[static_cast<size_t>(partial)].mode.get(); }

	/// FOLLOWS THE MODE: in High-pass, Width, Depth and Env Depth shape nothing (the brickwall has one shape), so
	/// they grey — the honesty rule — and the scope drops their grips. Read off the host's field, like the card.
	bool updateContent() override;

private:
	/// One partial's detail, all built together: its ratio row, its five knobs and its Mode chip.
	struct Detail {
		std::unique_ptr<HarmonicRow> row;
		std::vector<std::unique_ptr<Knob>> knobs;
		std::vector<fx::harm::Param> knobParams;
		std::unique_ptr<SwitchChip> mode;
	};
	struct Layout;
	Layout layout() const;
	Layout layoutFor(float totalWidth) const;
	DeviceControl& control(fx::harm::Param param) const;
	void followState();
	/// A cheap signature of every field the picture reads, so a tick redraws only when something moved.
	std::vector<float> signature() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	int selected_ = 0;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	std::unique_ptr<HarmPartialStrip> strip_;
	std::array<Detail, fx::harm::kPartialCount> detail_;
	std::vector<std::unique_ptr<ValueBox>> envelopeBoxes_;
	std::unique_ptr<HarmEnvelopeView> envelopeView_;
	std::unique_ptr<ValueBox> porta_;
	std::unique_ptr<SwitchChip> poly_;
	std::unique_ptr<SwitchChip> depthEnv_;
	std::unique_ptr<HarmScope> scope_;
	std::vector<float> drawnSignature_;
	std::vector<Caption> captions_;
};

} // namespace feathers::ui
