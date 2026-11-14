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
#include "plugin/ui/fx_card.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// HARM'S CARD — the fx lane's second tenant on the glass, and the first proof that the lane draws a
// device rather than draws the filter.
//
// IT REPLACES A PLACEHOLDER RATHER THAN A BLANK. NoEditorCard existed because HARM was audible and
// addable and had no editor, and its own comment says it goes away by itself: ensureCard builds one
// only for a device whose card this build cannot construct.
//
// THREE THINGS THIS CARD MUST NOT DO, each already paid for elsewhere:
//
//   - IT MUST NOT SAY "HPF". The firmware calls the notch's width knob `hpf` across thirteen
//     identifiers and its comment calls it a cutoff. It is neither: it is WIDTH, inverted, and
//     carrying that name onto the glass would put the lie somewhere a player reads it.
//   - IT MUST NOT OFFER TO SET THE NOTCH CENTRE. There is no such control. The centre is derived per
//     buffer from note + harmonic + fine + bend, and it falls back to the fundamental when the
//     oscillator is silent — see notchCentreHz. A box a hand could drag would be inventing a control
//     the device does not have.
//   - IT MUST NOT IMPLY POLY WORKS — AND THAT DAY CAME, WHICH IS THE INTERESTING PART. HARM declared
//     no perVoiceControl when this card was written, so the switch was drawn DISABLED and captioned
//     "per-voice not built yet". The road landed, the device's declaration followed it, and **the
//     switch came alive without this file being edited**: `polyIsLive` asks the descriptor rather
//     than holding an opinion, and the caption is a function of the same answer. That is the whole
//     value of deriving a control's liveness from the device instead of hardcoding it — the honest
//     state was maintained across the change by nobody remembering to.
//
// AND WHY THERE IS NO PICTURE OF THE NOTCH, which is the obvious thing to want and the reason it is
// absent: the notch's frequency depends on the NOTE BEING PLAYED, and a card has no note. Drawing it
// against a pretend middle C would be a curve that is wrong for every other note — the filter's curve
// is honest because a filter's corner is a setting, and HARM's is not. What the card shows instead is
// the RATIO, which is the part that is actually a setting: "3:2" is true at every pitch.

namespace feathers::ui {

class HarmCard final : public FxCard {
public:
	/// THE DEVICE IS TAKEN AT CONSTRUCTION, BY ID, not set afterwards, because a DeviceControl holds the
	/// device it was built for and a Knob holds the DeviceControl. Re-pointing a live card would mean
	/// re-pointing every adapter under it, and the lane already builds a fresh card whenever a device
	/// changes — so the mutable version would be a road with no caller and one more way to be wrong.
	HarmCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	HarmCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// THE DEVICE THIS CARD DRAWS, so the head can be derived rather than hand-wired — see
	/// FxCard::descriptor.
	const device::Descriptor* descriptor() const override { return &fx::harm::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	/// ASK FOR A FRAME WHEN THE BAND POSITIONS MOVE.
	///
	/// **THE HARMONIC ROW IS A CHILD WIDGET READING A CONTROL, AND NOTHING TOLD IT TO REPAINT.** The HARM
	/// band positions, and really every card widget, must redraw when what they show moves.
	/// It repainted on its own presses and on anything that happened to repaint the whole card —
	/// modulation arriving, a preset load, or the expanded panel moving the same control — went
	/// unnoticed. Redrawing the ROW rather than the card, for the reason `AutomodCard::update` gives
	/// about repainting a head and seven knobs to move one thing inside a child.
	bool updateContent() override;

private:
	double drawnHarmonic_ = -1.0;
	bool drawnEnabled_ = false;
	bool haveDrawnHarm_ = false;

public:
	void resized() override;

	//--------//
	// Layout //
	//--------//

	/// The same 24px head every docked card wears — a device and a macro card have to read as the
	/// same furniture.
	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kPad = 10.0f;

	/// The harmonic row: two arrows and the ratio between them. The filter's type row's shape, for
	/// the same job — stepping through a list that is too long for a hand to dial on a knob.
	static constexpr float kHarmonicRowHeight = 24.0f;
	static constexpr float kArrowWidth = 20.0f;

	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;

	/// FIVE cells per row, FIXED, so a knob keeps its cell whatever else is on the card — the rule
	/// FilterCard's macro row records having learned.
	///
	/// IT WENT THREE → FOUR → FIVE, AND EACH STEP HAD A REASON. Eight knobs in threes was a row with
	/// a hole in it at 330px; compressed, two rows of four made it 250. Then the approved depth
	/// control made NINE, and nine does not fit a 2×4.
	///
	/// FIVE CELLS IN BOTH ROWS, WITH THE TOP ROW LEAVING ITS LAST ONE EMPTY. The alternatives were
	/// worse: a third row gives back most of the 80px just won, and rows with DIFFERENT cell counts
	/// would draw the same knob at two sizes on one card. This keeps the card at 250px and keeps each
	/// row one half of the device — the oscillator above, the notch and the times below.
	///
	/// WHAT IT COSTS IS WIDTH: cells go 54px → 43px for a 38px knob, so the margin either side is
	/// 2.6px. That is the tightest grid on any card here and it is the part worth a human eye — see
	/// the hands-on checklist.
	/// THE CONTROLS THIS CARD PUTS A KNOB ON, DECLARED RATHER THAN COUNTED IN TWO PLACES.
	///
	/// THE SPLIT, BY DESIGN: every control and the filter-style filter-and-harmonic visualizer in the
	/// expanded view; only Level, Harmonic, Attack and Release on the card. Harmonic is the ROW, not a knob, so this
	/// is three — and the other six knobs and the Poly switch live in `HarmExpanded`.
	///
	/// IT IS A LIST AND NOT ARITHMETIC because `place(0, 4, rowA); place(4, 5, rowB)` is exactly what
	/// broke when Depth arrived. Everything below counts THIS: the constructor builds from it, the
	/// layout walks it, and `openHeight` sizes from its length. Adding a knob back is one entry.
	struct CardKnob {
		fx::harm::Param param;
		/// SPELLED OUT rather than derived from `ParamDesc::name`. The descriptor says "Harm Level"
		/// — qualified by the device, for the reason device.h states — and a card that stripped a
		/// five-character prefix would be doing string arithmetic on a name it does not own.
		const char* label;
	};

	static constexpr CardKnob kCardKnobs[] = {
		{ fx::harm::Param::Level, "Level" },
		{ fx::harm::Param::Attack, "Attack" },
		{ fx::harm::Param::Release, "Release" },
	};
	static constexpr size_t kCardKnobCount = std::size(kCardKnobs);

	/// HOW MANY KNOBS GO ACROSS. It was FIVE, for nine knobs in two rows, and the 43px cells that
	/// produced were a hands-on complaint of their own. Three knobs need three cells and the cells
	/// come out at ~68px for a 38px knob — so the reduction is the FIX for that item rather than
	/// something to be traded against it.
	static constexpr int kRowCells = static_cast<int>(kCardKnobCount);


	static float openHeight();
	static constexpr float foldedHeight() { return kHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }

	void setModulation(ModState* state, EditHistory* history) override;


	//------------------//
	// For the cases    //
	//------------------//

	/// Geometry asked rather than photographed — the rule this repo states about believing a layout.
	visage::Bounds headBounds() const;
	/// WHERE THE HARMONIC ROW SITS ON THIS CARD. The row's own rectangles are ITS business, in its
	/// own coordinates — a case that wants the arrows asks `harmonicRow()`.
	visage::Bounds harmonicBounds() const;

	/// The shared control itself, so a case drives the real widget rather than a rectangle it
	/// restated — and so the two surfaces can be asserted to hold the SAME kind of thing.
	HarmonicRow* harmonicRow() const { return row_.get(); }

	/// THE NOTCH / HIGH-PASS SWITCH (backlog NL), at the right of the harmonic row: the row says WHICH harmonic,
	/// the chip says what is done to it. A chip, because it has two states and names the one it is in.
	visage::Bounds modeBounds() const;
	SwitchChip* modeForTest() const { return mode_.get(); }

	/// THE ROW OF FOUR ACTIVE CHIPS AND EACH ONE'S RECTANGLE — drawn, laid out and hit-tested from this one answer.
	static constexpr float kActiveRowGap = 6.0f;
	visage::Bounds activeBounds(int partial) const;
	SwitchChip* activeChipForTest(int partial) const { return activeChips_[static_cast<size_t>(partial)].get(); }
	static constexpr float kModeChipWidth = 66.0f;

	/// THE ENABLE SQUARE, in the head, inboard of the slot menu — the filter card's, in the same place
	/// and drawn the same way, because a player should not have to learn two switches.
	///
	/// IT DID NOT EXIST UNTIL IT WAS DECIDED. This card dimmed on `Level > 0 || Width > 0` and said so,
	/// on the grounds that a control the DEVICE does not declare is one the honesty rule forbids
	/// drawing. The answer turned out to be that the device was wrong: it has the same active/inactive
	/// switch as every other effect, and the hardware-specific "level = 0 means off" semantics are gone.

	/// The parts a click can land on, so a case can drive the real widget at the real place rather
	/// than at a rectangle it restated — FilterCard's Part, for FilterCard's reason.
	/// THE HARMONIC ROW'S THREE PARTS ARE GONE FROM HERE. It is a child frame now (`HarmonicRow`),
	/// so its presses never reach the card's hit test at all — which is the point of extracting it,
	/// and is why `partAt` no longer has to be told about the arrows or the box.
	enum class Part { None, Fold, Enable, Expand, SlotMenu };
	Part partAt(visage::Point position) const;

	/// THE WAY INTO THE EXPANDED VIEW, in the head beside the enable square — and EMPTY for a device
	/// with no expanded view to open, so an empty rectangle refuses the press without a second branch
	/// (the enable square's own trick).
	///
	/// IN THE HEAD ON PURPOSE, KNOWING IT MOVES. The head is the natural home for a per-device verb
	/// and stage 4 rewrites the head — so this will be relocated. The alternative was a temporary
	/// home in the body, which is a control built, learned and then removed: two user-visible moves
	/// instead of one, to avoid a rebase within a head this session already owns.

	/// What the ratio box says, so a case can assert the STRING rather than take a picture of it.
	/// `ratioText` is the bare "3:2" — what a FOLDED head has room for. `harmonicText` is the whole
	/// label the open row shows, ratio and interval together.
	std::string ratioText() const;
	std::string harmonicText() const;
	/// The list behind the ratio box, beside the arrows.
	/// What the caption beside the poly switch says. It is not a constant: while the switch is dead
	/// the caption is the REASON, which is the whole of the honesty claim and therefore the thing a
	/// case has to be able to read.
	/// Whether the poly switch is live. False while HARM declares no per-voice control.

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// EVERY CONTROL THIS CARD PUTS SOMETHING A HAND CAN REACH ON — the card's own answer, not a list
	/// a case keeps beside it.
	///
	/// FilterCard's addressedParamsForTest, and it exists for the failure that one found: the gap
	/// nothing looks for is a DECLARED CONTROL WITH NO CONTROL ON THE GLASS. Walking the widget tree
	/// can only find widgets that exist; walking `kParamDescs` and asking this finds the control that
	/// does not — which is how ParamId::FilterPoly shipped with the engine reading it and no hand
	/// able to set it.
	///
	/// POLY IS IN THIS LIST AND IS DELIBERATELY NOT LIVE. Addressed is not the same as enabled: the
	/// card draws a switch for it, disabled and captioned with why, which is the honest answer while
	/// HARM declares no per-voice road. See polyIsLive.
	std::vector<fx::harm::Param> addressedParamsForTest() const;

private:
	void layOut();
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::toggleEnable.
	device::Modulatable* enableModulatable() override;

	void toggleEnabled();
	DeviceControl& control(fx::harm::Param param) const;

	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::harm::Param> knobParams_;
	std::unique_ptr<HarmonicRow> row_;
	std::unique_ptr<SwitchChip> mode_;
	/// THE FOUR PARTIALS' SWITCHES, in one small row under the knobs: a partial is switched on or off from the card, and the
	/// window (which has the tabs, the strip and the rest) is where it is shaped.
	std::array<std::unique_ptr<SwitchChip>, fx::harm::kPartialCount> activeChips_;
};

} // namespace feathers::ui
