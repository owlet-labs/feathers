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

#include "plugin/ui/widget_registry.h"
#include "fx/filter/filter_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"

#include <array>
#include <memory>
#include <vector>

// THE MULTIMODE FILTER'S CARD — the fx lane's first tenant on the glass, and the only surface it
// has. This device needs no expanded view. The module
// declares that (kTraits.hasExpanded is false) so the lane can ask rather than assume, and nothing
// here builds a second view.
//
// WIDTH IS RE-DERIVED, NOT COPIED. A card in this lane is FxLane::kWidth minus the lane's own
// padding on both sides. The modulator strip's cards are 216 wide because ITS rail is 236 — the
// same arithmetic, a different rail — and a 216 written here as a constant would be right by
// coincidence until somebody changed one of the two lanes.
//
// THE CURVE IS THE FILTER THAT PLAYS. Its points come from the module's own response evaluator,
// which is checked against the measured impulse response of the real sample loop in
// tests/test_fx_filter.cpp. What is drawn is the LINEAR response: drive is a waveshaper and has no
// magnitude response, so the curve does not pretend to show it.
//
// IT CARRIES WAVE DISPLAY'S TWO HARD RULES, both copied deliberately rather than rediscovered:
//
//   - COLOUR IS QUANTISED INTO BANDS and only pushed when the band changes. A per-sample colour puts
//     one gradient-atlas entry per point on every redraw, and a redraw happens on every parameter
//     change — which is every frame of a knob drag. That is the family this project traced its atlas
//     corruption to (wave_display.cpp:305-308).
//   - EVERY COMPUTED POINT IS FINITE-CHECKED before it reaches the renderer
//     (wave_display.cpp:341-343). These coordinates are COMPUTED — from a response evaluated at a
//     frequency — rather than placed from a sanitised parameter, which is exactly the case that rule
//     exists for. The evaluator has a case saying its points are finite; this checks anyway, because
//     the renderer is where a garbage quad lands and there is no loop condition to stop it.

namespace feathers::ui {

class FilterCard : public FxCard {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	/// THE DEVICE GOES IN — BY ITS ID (kill-lanes step 2), for the reason HARM's, OWLTT's and WAYPOINT's
	/// cards already say in FxLane::ensureCard: this card's controls are addressed by (device, control
	/// index), so a card that assumed one address edits the wrong device the moment the lane holds two.
	/// It did. Two filters in one chain mirrored each other — both cards wrote row 0 and both drew row
	/// 0 — because this was the one card whose constructor never took its address and whose member was
	/// declared `= 0` and assigned nowhere. A single filter anywhere but slot 0 was wrong too.
	/// **THROUGH THE SEAM, LIKE EVERY OTHER CARD.** This was the last card still taking a
	/// `ParamBridge`, which is the type only Feathers has — so the rack fell back to a `GenericCard`
	/// for the filter and drew a list of knobs where a curve belongs.
	FilterCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	           Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	FilterCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// The device this card is the face of. Test-only, and it exists because the identity is not
	/// recoverable from anything the card draws: a card on the wrong device renders a perfectly
	/// valid filter, just somebody else's.
	DeviceInstanceId deviceForTest() const noexcept { return device_; }
	/// Move one of this card's own controls, the way its knob does.
	void setControlForTest(fx::filter::Param param, double value) { control(param).setValue(value); }
	/// THE HEAD'S HAND-DRAWN CONTROLS, in card coordinates — they have no frame of their own, so a case that
	/// presses one through the tree has to be told where it is (backlog KR).
	visage::Bounds typeMenuBoundsForTest() const { return typeMenuBounds(); }
	visage::Bounds arrowBoundsForTest(int direction) const { return arrowBounds(direction); }
	visage::Bounds slopeBoxBoundsForTest() const { return slopeBoxBounds(); }

	/// THE DEVICE THIS CARD DRAWS, so the head can be derived rather than hand-wired — see
	/// FxCard::descriptor.
	const device::Descriptor* descriptor() const override { return &fx::filter::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	void resized() override;

	//--------//
	// Layout //
	//--------//

	/// The mock's card head, the same 24px the modulator cards use — a docked device and a macro card
	/// have to read as the same furniture.
	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kPad = 10.0f;

	/// The type row: two arrows and the dropdown between them.
	static constexpr float kTypeRowHeight = 24.0f;
	static constexpr float kArrowWidth = 20.0f;

	/// The response curve. Tall enough that a resonant peak has somewhere to be.
	static constexpr float kCurveHeight = 76.0f;

	/// The knobs, at the Engine rail's size — this is a card in a rail, not the footer.
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;

	/// The keytrack switch, which is an ARCHITECTURE and so gets words rather than a dot. It sits in a
	/// STRIP OF ITS OWN ALONG THE BOTTOM of the card, below the macro row, with the full word beside it.
	///
	/// IT HAS NOW LIVED IN THREE PLACES and each move fixed something the last one taught. In the
	/// macro row's third cell it was fine until Spread became a param and the row needed all three
	/// cells for knobs. In the head it was clear of everything but sat inside the head's own
	/// rectangle, whose verb is FOLD — a switch that folds the card if the hit test reaches it first.
	/// At the bottom it is in nobody else's rectangle, which removes that hazard rather than managing
	/// it, and it is below the fold, so it hides with the knobs like an ordinary control.
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kSwitchWidth = 44.0f;
	/// How many cells the macro row is divided into: Spread, Macro 1, Macro 2. FIXED, and each knob
	/// keeps its own cell whether or not the type declares it — a row that closed up around an absent
	/// knob would move the others under the pointer every time the dropdown changed.
	static constexpr int kMacroRowCells = 3;

	/// The slope box, at the right of the TYPE ROW, because a slope is an attribute of the type — the
	/// same reason it greys where the type declares only one.
	static constexpr float kSlopeBoxWidth = 46.0f;

	/// How tall the card is when open, and when folded. Both derived from the parts above, so a row
	/// that grows moves the card rather than overflowing it.
	static float openHeight();
	static constexpr float foldedHeight() { return kHeadHeight; }

	/// FOLDS, and participates in the lane's scroll by being a plain child frame whose height the lane
	/// reads. Same contract as the modulator cards.
	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	//------------//
	// Modulation //
	//------------//

	/// HAND THE CARD'S KNOBS THE MODULATION PANEL'S STATE, so a filter parameter can be armed to like
	/// any other destination: the knob glows while a source is armed, a drag on it writes an amount
	/// instead of a value, and the ring draws what reaches it.
	///
	/// Forwarded to the knobs rather than taken in the constructor, on the terms Knob::setModulation
	/// states. It had to be added at all because this card was written while no filter parameter was a
	/// destination, so its knobs were the only ones in the editor never told — and a knob that is
	/// never told behaves exactly as it did before the panel existed, which is why the gap was
	/// invisible from here: arming a source simply lit nothing on this card.
	void setModulation(ModState* state, EditHistory* history) override;

	/// THE LANE'S QUESTION, answered by the card's own two heights. Folded is the head alone — the
	/// mock's `.mod-card.min` one chain over — and the lane stacks whichever it is told.
	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }

	/// Where the keytrack switch and its caption sit, so a case can ask whether they overlap anything
	/// rather than take a picture and squint. Both are in the bottom strip.
	visage::Bounds polyBounds() const;
	visage::Bounds polyLabelBounds() const;
	visage::Bounds keytrackBounds() const;
	visage::Bounds keytrackLabelBounds() const;
	/// The top of that strip, which is also what the card's height is derived from.
	float polyRowTop() const;
	float keytrackRowTop() const;

	/// Where the slope box sits on the type row. Drawn greyed and unclickable for a type that declares
	/// only one slope, rather than vanishing: a box that disappears would move the row under the
	/// pointer as the dropdown changed, and the greyed box says the true thing — this type has one.
	visage::Bounds slopeBoxBounds() const;
	/// Does the slope control do anything for the type showing now? The module owns the answer; this
	/// asks it rather than keeping a second list.
	bool slopeIsLive() const;

	/// The parts a click can land on, so a case can drive the real widget at the real place rather
	/// than at a rectangle it restated — the pattern ExploreStage::partBounds established.
	enum class Part { None, Fold, Enable, TypeMenu, PrevType, NextType, SlopeMenu, SlotMenu };

	Part partAt(visage::Point pixels) const;
	visage::Bounds headBounds() const;
	visage::Bounds typeMenuBounds() const;
	visage::Bounds arrowBounds(int direction) const; ///< -1 previous, +1 next
	visage::Bounds curveBounds() const;

	//-------//
	// Types //
	//-------//

	/// The dropdown's entries: every real type, then the families that are coming as DISABLED "· soon"
	/// rows — the HeaderBar idiom, which is also what gives the arrows something honest to skip.
	struct TypeEntry {
		const char* name;
		int type;     ///< an fx::filter::Type, or -1 for an entry that cannot be chosen
		bool enabled;
	};
	static std::span<const TypeEntry> typeEntries();

	/// The type an arrow would move to, or -1 if there is none that way. Skips the disabled entries
	/// rather than stopping on them, which is the whole reason they are marked rather than hidden.
	int typeAfter(int type, int direction) const;

	/// What the two macro knobs are called RIGHT NOW. The labels follow the dropdown; the ids never
	/// move. Public so a case can read what the glass says rather than restate the mapping.
	const char* macroLabel(int which) const;

	/// Show a macro knob only where the type declares a meaning for that slot, and hide it where the
	/// type declares MacroKind::None. Called wherever the answer can change — the type moving under
	/// the dropdown, and the card folding — so there is one rule rather than two that must agree.
	void syncMacroKnobs();

	/// The slope entry showing now, and the menu behind the box. The menu offers every entry and
	/// DISABLES the ones this type does not declare, which is the same shape the type menu uses for
	/// its "· soon" rows — a list that hides what it cannot offer teaches nothing about why.
	int selectedSlope() const;
	void showSlopeMenu();
	void chooseSlope(int slope);

	//-----------//
	// The curve //
	//-----------//

	/// HOW FINELY THE RESPONSE IS SAMPLED, as points per LOGICAL pixel of curve width, multiplied by
	/// the display scale — WaveDisplay's own convention (wave_display.h:67-71) and its own constant,
	/// because there is one right answer to "how many points is a drawn curve" in this editor and it
	/// should not be two.
	///
	/// IT SHIPPED AT 96 POINTS AND THAT WAS WRONG, which is worth recording because the number looked
	/// perfectly reasonable. 96 log-spaced points from 20 Hz to 20 kHz step 7.5% in frequency each;
	/// a resonance at the SVF's 0.98 clamp is a Q of 25, whose peak is 4% wide — narrower than the
	/// step, so the peak fell BETWEEN two samples and the drawn curve missed it by 1.97 dB. At this
	/// convention the same peak is drawn to within 0.08 dB. Measured both ways; see the case.
	static constexpr float kPointsPerPixel = 2.0f;

	/// Never fewer than this however narrow the card gets — a floor on the frequency resolution rather
	/// than on the pixel one, since it is the RESONANT PEAK that needs the points and its width does
	/// not depend on how wide the card is.
	static constexpr size_t kMinCurvePoints = 601;

	/// The ceiling, so a wildly scaled display cannot make one draw walk an unbounded buffer.
	static constexpr size_t kMaxCurvePoints = 2048;

	/// How many points this card will draw right now.
	size_t curvePointCount() const;

	/// The curve the card would draw, in linear amplitude, at the settings the parameters hold.
	/// Public for the same reason the wave display publishes its points: a case should read what
	/// reached the glass rather than recompute it. Writes up to `capacity` entries and returns how
	/// many it wrote.
	///
	/// `right` (backlog KU): while the plot draws an ear each (`stereoCurves()`), the RIGHT ear's curve; the left's
	/// otherwise. In mono the argument changes nothing — there is one curve, from the base.
	size_t curveGains(float* out, size_t capacity, bool right = false) const;

	/// **AN EAR EACH** (backlog KU, Q1): true while a stereo route reaches Cutoff, Resonance, Macro 1 or Macro 2 —
	/// then the plot draws two curves from the MODULATED rows, left and right. Otherwise it draws the base, as it
	/// always has.
	bool stereoCurves() const;
	bool stereoCurvesForTest() const { return stereoCurves(); }
	size_t curveGainsForTest(float* out, size_t capacity, bool right) const { return curveGains(out, capacity, right); }

	/// The knobs this card holds, so a case can ask where they ended up rather than restate the
	/// layout it is checking.
	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// The keytrack switch, for the same reason: a case asks the control how it reads and where it is
	/// rather than taking a picture of the card and squinting at it.
	SegmentedControl* keytrackForTest() const { return keytrack_.get(); }
	SegmentedControl* polyForTest() const { return poly_.get(); }

	/// EVERY PARAMETER THIS CARD PUTS A CONTROL ON — the card's own answer to "what can a hand
	/// reach", rather than a list a test keeps beside it.
	///
	/// It exists because the gap nothing looked for is a PARAM WITH NO CONTROL. Walking the widget
	/// tree can only find controls that exist; walking the declarations and asking this finds the one
	/// that does not, which is how ParamId::FilterPoly shipped for a build with the engine reading it
	/// and no hand able to set it.
	std::vector<fx::filter::Param> addressedParamsForTest() const;

	/// THE SETTINGS THE CURVE IS DRAWN FROM. Exposed because "the curve is the filter that plays" is
	/// this card's stated promise, and a promise about an assembly needs the assembly to be readable:
	/// a case can then compare it field for field against the engine's own, which is the only check
	/// that catches a control the card forgot to pick up rather than one somebody thought to sample.
	fx::filter::Settings settingsForTest() const { return settings(); }

private:
	fx::filter::Settings settings() const;
	/// ONE EAR'S SETTINGS, from where modulation left each control (backlog KU) — the left ear's `modulatedFieldAt`,
	/// the right's `modulatedFieldRAt` wherever a stereo route reaches, the left's elsewhere.
	fx::filter::Settings settingsFor(bool right) const;

	/// ASK FOR A FRAME WHEN THE PICTURE THIS CARD DRAWS HAS MOVED.
	///
	/// **THE CARD HAD NO SUCH ROAD, SO A KNOB MOVED THE VALUE AND NOT THE CURVE.** Moving the cutoff
	/// knob did not move the drawn curve, though both moved with the XY pad and with modulation.
	/// **That difference IS the diagnosis**: a drag on a pad repaints the card as
	/// a side effect of the gesture, and modulation arrives through a road that already redraws — while
	/// a knob repaints only itself. So the picture followed two of its three inputs.
	bool updateContent() override;

private:
	/// WHAT THE PICTURE ON SCREEN WAS DRAWN FROM — compared against `settings()` once a tick.
	mutable fx::filter::Settings drawnSettings_ {};
	/// AND, WHILE AN EAR EACH IS DRAWN (backlog KU), the right ear's — so the card redraws while either ear moves.
	mutable fx::filter::Settings drawnSettingsR_ {};
	mutable bool drawnStereo_ = false;
	mutable bool haveDrawn_ = false;

public:
	int selectedType() const;
	void showTypeMenu();
	void chooseType(int type);
	void stepType(int direction);
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::toggleEnable.
	device::Modulatable* enableModulatable() override;
	/// The hand-drawn parts' tooltips — see `FxCard::contentTooltipAt`.
	const char* contentTooltipAt(visage::Point local) const override;

	void toggleEnabled();
	/// Tell the controls the card holds whether the device is in the signal, so they draw switched off
	/// while it is not. Told rather than asked because a Knob has no way to reach the card's parameter.
	void showEnablement(bool on);
	void layOutKnobs();

	/// ONE ADAPTER PER DECLARED CONTROL, built once and outliving every widget that points at one.
	///
	/// A widget holds a `device::Modulatable&` and does not own it, so these have to live somewhere
	/// that outlasts the knobs — and the card is the only thing that does. Built for ALL of the
	/// device's controls rather than only the ones with widgets, because the three drawn as bespoke
	/// parts (the enable square, the type dropdown, the slope dropdown) address their values through
	/// exactly the same road as the knobs do; the difference between them is what draws, not how it
	/// is reached.
	DeviceControl& control(fx::filter::Param param) const;

	backplane::DeviceHost& host_;
	/// **HELD, NOT ONLY FORWARDED.** `setModulation` used to hand these straight to the widgets; a
	/// `DeviceControl` takes them at CONSTRUCTION and the adapters here are built lazily, so one built
	/// after a `setModulation` would have missed it and reported itself unarmed forever.
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	Notifier* notifier_ = nullptr;

	/// WHICH DEVICE THIS CARD IS THE FACE OF — its id, wherever in the chain it is drawn.
	const DeviceInstanceId device_;

	mutable std::array<std::unique_ptr<DeviceControl>,
	                   static_cast<size_t>(fx::filter::Param::kCount)> controls_;

	std::vector<std::unique_ptr<Knob>> knobs_;
	/// The control each knob in `knobs_` was built for, in the same order — see addressedParamsForTest.
	std::vector<fx::filter::Param> knobParams_;
	std::unique_ptr<SegmentedControl> poly_;
	std::unique_ptr<SegmentedControl> keytrack_;
};

} // namespace feathers::ui
