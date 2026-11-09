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

#include "fx/automod/automod_derived.h"
#include "fx/automod/automod_types.h"
#include "plugin/ui/automod_strip.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <span>
#include <vector>

// THE AUTOMODULATOR'S CARD — three coordinates, one phase, five performance knobs.
//
// TWO ROWS, AND THE SPLIT IS THE DEVICE'S OWN SHAPE RATHER THAN A FITTING PROBLEM. The top row is
// WHERE THE SOUND COMES FROM — Type, Flavour and Mod are the coordinates the whole character layer is
// read from, and Gamma moves all three at once. The bottom row is WHAT PLAYS IT — rate, mode, depth,
// manual, mix. Nothing in the bottom row changes what the device IS; nothing in the top row is a
// performance gesture.
//
// GAMMA IS A PLAIN `ValueBox` AND THE THREE COORDINATES ARE `ZoneCoordBox`es. Gamma is coarse
// only, so it is a value box. The reason generalises: a
// `ZoneCoordBox`'s coarse:fine split exists so a hand can land on a NAMED region — "Open", "Rest" —
// with the fine half positioning you inside it. A device-wide phase has no such vocabulary, so there
// is nothing for a fine half to be fine ABOUT. `automod_types.h` carries the same paragraph beside
// the declaration, which is where somebody reading the device rather than the card will look.
//
// AND ABOVE BOTH ROWS, THE LFO STRIP — the picture the decline note used to promise.
//
// THE OLD NOTE HERE SAID "NO PICTURE, AND THAT IS A DECLINE RATHER THAN AN OVERSIGHT", naming exactly
// what was missing: the LFO's waveform as the Mod coordinate turns. This is it — an LFO shape preview
// with the filter curve overlaid. `hasExpanded` stays false:
// a strip on the card is a picture, not a second surface, and declining an expanded panel still costs
// nothing.
//
// **IT DRAWS WHAT THE DSP WILL DO, RATHER THAN A SECOND IMPLEMENTATION OF IT**, which is CLAUDE.md's
// rule pointed at a picture and is the whole of why three functions moved out of `automod.cpp` to
// make it possible. The strip calls `coordsOf`, `deriveAutomod`, `evalLfoShape`, `modulationFor` and
// `cornerHzFor` — every one of them the function the audio path itself calls. There is no arithmetic
// in this file that the sound does not also run.
//
// WHY IT SITS ABOVE THE COORDINATES rather than below them: the strip is what the top row AUTHORS.
// Type, Flavour and Mod redraw the waveform; the bottom row plays it. Reading downward the card now
// says "here is the shape / here is what makes it / here is how it is played".

namespace feathers::ui {

class AutomodCard final : public FxCard {
public:
	AutomodCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	            Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	AutomodCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	/// WHETHER THIS DEVICE IS IN THE SIGNAL — the head's switch asks, and `FxCard` draws it. The
	/// drawing was this card's until it was seven identical copies; only the READ is per-card,
	/// because only the card knows its own control road.
	bool headEnabled() const override;
	void resized() override;
	bool updateContent() override;
	float laneHeightAt(float) const override { return folded_ ? foldedHeight() : openHeight(); }
	void setModulation(ModState* mod, EditHistory* history) override;
	const device::Descriptor* descriptor() const override { return &fx::automod::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;

	/// HOW TALL THE LFO STRIP IS. Drawn's response curve is 62 and this is the same number for the
	/// same reason: tall enough that a waveform's corners are distinguishable, short enough that it
	/// does not compete with the controls for the eye.
	static constexpr float kStripHeight = 62.0f;

	/// THE GUTTER BETWEEN TWO PICTURES SHARING ONE ROW. Unused while both traces share one box, and
	/// kept because the side-by-side candidate is one line away if it is ever wanted again.
	static constexpr float kStripGutter = 8.0f;

	/// **SIX PERFORMANCE KNOBS ON ONE ROW WAS A REAL DEFECT, and its lesson survives the regrid.**
	/// MEASURED from a photograph: at six the cells were 32px against a 38px face, and `knob.cpp:551`
	/// centres a caption at `(width - captionWidth) * 0.5f`, which goes NEGATIVE when the caption is
	/// wider than the cell — so an over-packed row spills BOTH ways rather than clipping. The label
	/// line read `RATE SYNC :O MOC DEPTH ANUAL MIX`.
	///
	/// **SHORTENING THE NAMES WAS THE OTHER CANDIDATE AND IT IS THE WRONG ONE.** `ParamDesc::name` is
	/// what the HOST shows in its automation lane; trimming `Manual` to `MAN` to fit a card would make
	/// every DAW's parameter list worse to fix one surface's arithmetic. The name is the device's, the
	/// layout is the card's.
	///
	/// MEASURED CAPTION WIDTHS, which is what decides the grids below: `MIX` 17.5, `RATE`/`SYNC` 25.3,
	/// `FREQ` 26.3, `DEPTH` 34.0, `MANUAL` 41.8 — and **`LFO MODE` 53.32 against a four-wide cell of
	/// 54.** Everything but that one fits a row of four with room; `LFO MODE` fits by 0.68px, which is
	/// not a fit.

	/// **THE COORDINATES GO TO THE BOTTOM AND THE PERFORMANCE CONTROLS COME UP** — Type, Flavor and Mod
	/// sit in the bottom row. Reading down, the card is now: the picture, the things that PLAY
	/// it, then the things that AUTHOR it.
	///
	/// TWO GRIDS FOR THE PERFORMANCE GROUP, because Freq made it seven and seven does not divide.
	/// Both were photographed before either was chosen — see `Layout`.
	enum class Layout {
		/// Three to a row throughout: 3 / 3 / 1. Columns line up down the card; the last row is a
		/// single knob beside a gap.
		Uniform,
		/// Three then four: 3 / 4. One row shorter, at the cost of two rows whose column pitch does
		/// not match (72px against 54px).
		Compact,
	};
	void setLayout(Layout which);
	[[nodiscard]] Layout layout() const noexcept { return layout_; }

	/// HOW MANY CELLS EACH PERFORMANCE ROW HOLDS, for the layout in force. The one place that decides
	/// the grid — `layOut` walks it and `openHeight` counts it, so a row cannot be laid out at one
	/// width and sized at another.
	[[nodiscard]] static std::span<const size_t> performanceRows(Layout which);

	/// The coordinate row: Type, Flavour, Mod and the device gamma.
	static constexpr size_t kTopCells = 4;
	/// Every performance control, counted from the array that builds them.
	static constexpr size_t kBottomCells = 7;

	/// **PER-INSTANCE, NOT STATIC, BECAUSE THE HEIGHT DEPENDS ON THE LAYOUT.** `GenericCard` is the
	/// precedent — its `openHeight` is `const` for the same reason, a card whose size is a property of
	/// what it holds rather than of its class.
	///
	/// The static form reserved the TALLER grid so the lane could ask without naming a card, and left
	/// `Compact` sitting a whole row short of what it had reserved — about 90px of dead space, plainly
	/// visible in the photograph. `laneHeight()` is already virtual and already per-instance, so the
	/// lane was never actually asking the class.
	[[nodiscard]] float openHeight() const;
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	[[nodiscard]] visage::Bounds headBounds() const;

	//---------------//
	// The LFO strip //
	//---------------//

	/// **THE STRIP IS ITS OWN FRAME NOW, AND THE CARD FORWARDS.**
	///
	/// `AutomodStrip` owns the picture — the traces, the derive gate, the two coordinate systems and
	/// the drawing. What stays here is what the CARD knows: where the picture sits on this surface
	/// (`lfoStripBounds`, below) and how tall it is.
	///
	/// **THE FORWARDERS ARE NOT CONVENIENCE.** A pure refactor is verified by the EXISTING cases
	/// passing UNCHANGED (playbook 8t-bis), and eight of them reach these names on the card. Keeping
	/// the names — and the TYPES, by alias, so `AutomodCard::Picture::Dual` still denotes the same
	/// enumerator — is what lets the move be checked rather than re-graded.
	using Picture = AutomodStrip::Picture;
	using ResponseStyle = AutomodStrip::ResponseStyle;
	using PreviewTrace = AutomodStrip::PreviewTrace;
	using ResponseTrace = AutomodStrip::ResponseTrace;
	using CornerTravel = AutomodStrip::CornerTravel;
	using View = AutomodStrip::View;

	void setPicture(Picture which) { strip_->setPicture(which); }
	[[nodiscard]] Picture picture() const noexcept { return strip_->picture(); }
	void setResponseStyle(ResponseStyle style) { strip_->setResponseStyle(style); }
	[[nodiscard]] ResponseStyle responseStyle() const noexcept { return strip_->responseStyle(); }

	/// The response's rectangle IN CARD COORDINATES — **the same box the waveform uses**, which is
	/// the whole of "same field". Empty when the picture does not include a response, and empty while
	/// folded, because a folded card has no picture at all.
	[[nodiscard]] visage::Bounds responseStripBounds() const;

	[[nodiscard]] ResponseTrace responseTrace() const { return strip_->responseTrace(); }

	/// The strip's rectangle in card coordinates. Empty while folded. **THIS ONE IS THE CARD'S OWN
	/// GEOMETRY AND IS NOT A FORWARDER** — the card decides where the picture goes and sizes the
	/// strip from this in `layOut`, so the two cannot disagree about how much room it took.
	[[nodiscard]] visage::Bounds lfoStripBounds() const;

	[[nodiscard]] PreviewTrace previewTrace() const { return strip_->previewTrace(); }
	[[nodiscard]] CornerTravel cornerTravel() const { return strip_->cornerTravel(); }
	[[nodiscard]] const View& view() const { return strip_->view(); }
	[[nodiscard]] size_t deriveCountForTest() const noexcept { return strip_->deriveCountForTest(); }
	[[nodiscard]] fx::automod::Settings settings() const { return strip_->settings(); }

	/// THE PICTURE ITSELF, for a surface that wants to talk to it directly. `OwlttCard::graph()` is
	/// the precedent.
	[[nodiscard]] AutomodStrip* strip() const noexcept { return strip_.get(); }

	//------------//
	// Test seams //
	//------------//

	/// EVERY CONTROL THIS SURFACE REACHES, which `test_gui_widgets.cpp` compares against the device's
	/// declaration. **BOTH HALVES OF EVERY COORDINATE BOX** — the box is two editors, not a knob with
	/// a gesture, so a surface claiming only the fine half would understate what a hand can touch.
	[[nodiscard]] std::vector<fx::automod::Param> addressedParamsForTest() const {
		std::vector<fx::automod::Param> out = knobParams_;
		out.insert(out.end(), choiceParams_.begin(), choiceParams_.end());
		out.insert(out.end(), coordParams_.begin(), coordParams_.end());
		out.push_back(fx::automod::Param::Gamma);
		out.push_back(fx::automod::Param::Enabled);
		return out;
	}

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& coordsForTest() const {
		return coords_;
	}
	[[nodiscard]] const GammaBox* gammaForTest() const { return gamma_.get(); }
	[[nodiscard]] const std::vector<std::unique_ptr<ValueBox>>& choicesForTest() const {
		return choices_;
	}

private:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt — one line, so the toggle, the
	/// gesture that records the undo step, and the redraw all live once in the base.
	device::Modulatable* enableModulatable() override;

	[[nodiscard]] DeviceControl& control(fx::automod::Param p) const;
	void layOut();

	/// THE PICTURE, AS A CHILD FRAME. **A parent told to repaint is not a child told to repaint**
	/// (playbook 8i), and the strip is about to be told per frame — so the frame that draws it is the
	/// frame that owns it. `OwlttCard::graph_` is the same arrangement for the same reason.
	std::unique_ptr<AutomodStrip> strip_;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<std::unique_ptr<ZoneCoordCell>> coords_;
	std::unique_ptr<GammaBox> gamma_;
	/// SYNC AND LFO MODE — enumerations, so boxes rather than knobs. See the constructor.
	std::vector<std::unique_ptr<ValueBox>> choices_;
	std::vector<fx::automod::Param> choiceParams_;
	/// Their cells, so `draw` can put each caption on the row's shared label line — the box is INSET
	/// inside its cell and does not know where that line is. Gamma's `gammaCell_` for the same reason.
	std::vector<visage::Bounds> choiceCells_;
	[[nodiscard]] bool isChoiceBox(const visage::Frame* frame) const;
	/// The gamma cell's rectangle, so `draw` can put its caption on the row's shared label line.
	/// Held because the box itself is INSET inside the cell and does not know where the row's label
	/// line is.
	visage::Bounds gammaCell_;

	/// THE CANDIDATE UNDER REVIEW. Both layouts are runtime-switchable so `widget-shot` can
	/// photograph every combination without a rebuild — the choice is made from pictures, which is how the
	/// first version was settled and how the auto-range was caught. The PICTURE's two candidates live
	/// on the strip with the drawing they select between.
	Layout layout_ = Layout::Compact;

	/// 1:1 WITH `knobs_`, read POSITIONALLY — an entry for a widget that is not in `knobs_` would
	/// silently answer the wrong control for every knob after it. The coordinate halves therefore go
	/// in their own vector, as the shaper's card records.
	std::vector<fx::automod::Param> knobParams_;
	std::vector<fx::automod::Param> coordParams_;
	std::vector<visage::Frame*> cells_;

	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
