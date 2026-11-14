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

#include "backplane/device_host.h"
#include "mod/dahdsr/dahdsr.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/rack_control.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "plugin/ui/rack_card.h"

#include <visage/ui.h>

// THE DAHDSR'S CARD — the device drawing itself, which is what a dockable device is entitled to do.
//
// THE LAYOUT, and the reasoning behind it is worth keeping because it is not the obvious one.
// A first sketch had six knobs in a 3x3 grid with the curves alongside; value boxes replaced them
// (a draggable, modulatable time box saves space against knobs), then two rows rather than six, and
// then the thing that made it good: ADSR on the top row, with defaults for the others such that a
// player need only touch ADSR if that is all they want.
//
// So the top row is the FAMILIAR FOUR and nothing else, and the second row holds the two stages an
// ADSR does not have plus the smoothing time — all three of which default to out of the way. A
// player who wants an ADSR never touches the second row and never has to know it is a DAHDSR. That
// claim is not a hope: tests/test_mod_dahdsr.cpp's "an untouched envelope IS an ADSR" asserts the
// SHAPE — first tick already climbing, no plateau before the decay — because "delay is at its
// minimum" would pass while the state machine still spent a tick in a delay stage.
//
// AND THE CURVE CARRIES THE THREE SHAPES. Six boxes cover the six times and levels, which leaves the
// attack, decay and release curves without a box — and they do not want one, because a number is a
// poor way to say what a curve looks like. The curve is draggable too. So the strip
// at the top is not a preview, it is the shape editor, and it is the reason the whole card fits.
//
// THE CURVE IS THE ENVELOPE THAT PLAYS, by construction rather than by test. The filter's card needed
// a closed-form response evaluator beside its sample loop and then a case proving the two agree; this
// needs neither, because an envelope is cheap and deterministic and can simply be RUN. There is one
// implementation of a DAHDSR in this program and the picture comes out of it.

namespace feathers::ui {

class EnvCard : public FxCard {
public:
	/// TAKES THE DEVICE SEAM, NOT THE PARAMETER ROAD. It took a `ParamBridge&`, which is Feathers'
	/// and Feathers' alone, so this card could not be built in the rack at all — half of the bug where
	/// the modulator devices did not render. Both hosts now hand it a `backplane::DeviceHost`,
	/// which is the same convergence the six FX cards made.
	EnvCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	EnvCard(backplane::DeviceHost&, size_t, ModState&, EditHistory&) = delete;

	/// **IT UNREGISTERS ITSELF AND ITS BOXES, BECAUSE IT REGISTERED BOTH.**
	///
	/// This is the case `forgetFrameTree`'s comment was written about — *"that list is joined by
	/// CHILDREN THE OWNER NEVER SEES"* — seen from the other side: here the owner DOES see them,
	/// because it put them on the list itself. The symmetric rule covers it exactly: whoever
	/// registers, unregisters. A box cannot do it alone, since it never knew it was watched.

	/// THIS CARD'S BODY. The frame, the head and the name are `FxCard::draw`'s — see the note there on
	/// why the standardized header is the default rather than a convention.
	void drawContent(visage::Canvas& canvas) override;

	/// WHICH SOURCE THIS CARD PUBLISHES, so the shared frame can draw its GLOW — not its border,
	/// which is selection's and is the base class's flag. See `FxCard::setFocused`.
	bool headSourceFocused() const override;
	bool headArmed() const override;
	void resized() override;

	/// **THE TRIG PAD** (backlog MV), seated at the shape's left, where the envelope starts: what it fires is the
	/// curve beside it. See `trig_pad.h`.
	[[nodiscard]] visage::Bounds trigPadBounds() const;
	[[nodiscard]] TrigPad* trigPadForTest() const noexcept { return trigPad_.get(); }
	/// TEST ONLY: the cached curve, and how many times it has been rebuilt.
	[[nodiscard]] const std::vector<visage::Point>& curvePointsForTest() const { return curvePoints(); }
	[[nodiscard]] size_t curveRebuildsForTest() const { return curveRebuilds_; }
	/// THE SLOT MENU'S DOTS, WHEN A HOST BINDS ONE (a lane: Feathers' fx lane holds modulators since 2026-10-04) —
	/// `drawSlotMenu` draws nothing where none is bound, so the strip, which draws its own head mark, is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }

	/// The strip asks this rather than the static height() below, because it holds cards of
	/// more than one device now and cannot name their types. See rack_card.h.



	/// ITS DESIGN WIDTH IN A HORIZONTAL LANE — **NOT the default, which would answer `laneHeight()`
	/// and hand a height back as a width.** The strip lays this card out at `kModCardWidth`; a lane
	/// that let the default stand drew it at whatever its height happened to be, which is how it
	/// arrived in the rack too narrow with its last row of chips clipped off the edge.
	///
	/// **AND IT ANSWERED THAT WIDTH ON THE VERTICAL AXIS TOO, WHICH IS THE SAME MISTAKE FACING THE
	/// OTHER WAY.** `laneExtent(cross)` is *"how much FLOW given this much CROSS"* — a height
	/// vertically, a width horizontally — and this override ignored the axis entirely, so a vertical
	/// lane asking how tall the card is was handed 216 because that is how WIDE it is. Nothing caught
	/// it because the two hosts that call this are the rack (horizontal, right answer) and the fx
	/// lane, which holds no modulator today. The modulator strip is the third, and routing it here is
	/// what made this reachable.
	float laneExtent(float cross) const override {
		// **A FOLDED CARD IS ITS SPINE, AND THIS LINE IS WHY MSEG WOULD NOT COLLAPSE.** It answered `kModCardWidth` unconditionally, so a folded card kept
		// its full 216 and the fold changed nothing a lane could see — the state flipped, the head
		// redrew, and the geometry never moved. The fold is not a card's own business to remember:
		// the extent has to report it.
		if (folded()) {
			return kCardHeadHeight;
		}
		if (laneAxis() == LaneAxis::Horizontal) {
			return kModCardWidth;
		}
		// **THE VERTICAL ANSWER IS THE BASE'S NOW, because the base's is `laneHeightAt(cross)`** — it
		// used to be `laneHeight()`, which discarded the cross this function exists to convey, and
		// restating the sum here would be a second spelling of `laneHeightAt` one edit from
		// disagreeing with it.
		return FxCard::laneExtent(cross);
	}

	/// HOW TALL THIS CARD'S BODY IS — the head row is NOT included, and that is a division rather than
	/// an oversight.
	///
	/// THE STRIP OWNS THE HEAD, EVERY DEVICE OWNS ITS BODY. The dot, the name, the arm arrow and the
	/// fold button are the same on every card in the strip, they are how a source is FOCUSED, and a
	/// device drawing its own would be a device deciding what the strip looks like. So this frame
	/// starts below them.
	///
	/// IT IS ALSO WHAT MAKES THOSE BUTTONS CLICKABLE. A first version had the card's frame cover the
	/// whole card including the head; the card is a child frame, so it sat on top of the fold and the
	/// arrow and swallowed every press aimed at them. The buttons were drawn, looked right, and did
	/// nothing — which a case caught only because it drove the real button at the real place rather
	/// than calling setCardMinimised.
	///
	/// DEFINED HERE rather than in the .cpp: constexpr implies inline, so a definition in one
	/// translation unit emits nothing for the others and the strip would fail to link.
	/// WHERE THE BODY STARTS. The card now frames ITSELF — `ui::drawCardFrame` — so its own head sits
	/// above everything it draws, and every geometry helper below measures from here rather than from
	/// zero. It was zero for as long as Feathers' strip owned the head and the card was a child frame
	/// positioned beneath it.
	/// WHERE THE BODY STARTS — **no longer a constant, because the head moved.** In a v-scroll lane
	/// the head is the top row and this is its height; in an h-flow lane the head is the LEFT spine and
	/// the body starts at the card's top with `bodyLeft()` taking the offset instead.
	float bodyTop() const { return bodyTopEdge(); }

	/// WHICH DEVICE THIS CARD IS. The base answers nullptr; a card that knows says so, and the shared
	/// frame reads the name and the output count from it rather than being told them.
	const device::Descriptor* descriptor() const override { return &mod::dahdsr::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// THE SOURCE ONE OF THIS DEVICE'S OUTPUTS PUBLISHES — its id and the output's own index, which is
	/// all a source is (kill-lanes step 2). It was a flat index computed from a lane, and the old note
	/// here warned that a card computing it from its own slot would be assuming which rack it was in;
	/// an id is in no rack's slot space at all.
	backplane::Source sourceOfOutput(size_t output) const {
		return backplane::Source::output(device_, output);
	}

	/// THE WHOLE CARD: head, body and the outputs strip its host fills. `height()` below is the BODY
	/// alone and stays that way — the strip's layout is written in terms of it.
	/// FOLDED, THIS CARD IS ITS HEAD AND NOTHING ELSE — the generic card's rule, which this card did
	/// not follow because the strip used to HIDE a minimised card rather than fold it. See
	/// `ModPanel::resized` and backlog CZ: a hidden card draws no head, so a folded modulator was a
	/// blank band a hand could not find its way back from.
	/// **THE FOOT IS `outputStripHeight()`, NOT ONE ROW OF IT.** It read `kCardOutputStripHeight`
	/// flat, which is exactly right for a device with one output and silently short for one with
	/// several — the same restatement `GenericCard::liveHeight` was carrying when SKLITTER arrived
	/// with seven. The DAHDSR declares one and always will as far as this card knows, so this is not
	/// a bug being fixed; it is the one-row assumption being deleted rather than copied (backlog FL).
	///
	/// **IT USED TO READ `width()`, WHICH `liveHeight`'s own doc NAMES AS THE TRAP** — a card the lane
	/// has not placed yet answers zero, and a zero-width strip cannot wrap, so it reports ONE row.
	/// **The precondition that made that safe was the DEVICE, not the timing**: the DAHDSR declares
	/// exactly one output, and one chip needs one row at every width including zero.
	///
	/// **THAT PRECONDITION WAS A CLAIM NOTHING CHECKED, AND ITS TWIN SHIPPED A DEFECT** —
	/// `GenericCard` reached the same `width()` with a device publishing seven and was given a one-row
	/// foot in the modulator strip. So the width is a PARAMETER now and there is no precondition left
	/// to state: this counts the rows at whatever width it is asked about, and a DAHDSR that grew a
	/// second output would simply be taller. A comment guarding an invariant is worth less than an
	/// invariant that cannot be broken.
	float laneHeightAt(float cardWidth) const override {
		return folded() ? kCardHeadHeight
		                : kCardHeadHeight + height() + outputStripHeightAt(cardWidth);
	}
	/// THE BOXES GO WITH THE BODY. `FxCard::foldedChanged`'s own warning: a child left visible under
	/// a folded head is still pressable, over whatever card the strip moved up.
	void foldedChanged() override;

	static constexpr float height() {
		return kGap + kCurveHeight + kGap + ValueBox::kHeight + kRowGap + ValueBox::kHeight + kGap
		       + kChipHeight + kGap;
	}

	//----------//
	// Geometry //
	//----------//

	/// WHICH CONTROLS SIT WHERE, in the order they are laid out. Public because the layout IS the
	/// feature itself, and a case that checked it by re-listing the order would be checking
	/// its own copy.
	static constexpr size_t kTopRow = 4;    ///< A D S R — the familiar four
	static constexpr size_t kBottomRow = 4; ///< Delay, Hold, Smoothing — all off by default — and Amount (DH)
	static constexpr size_t kBoxCount = kTopRow + kBottomRow;

	static const std::array<mod::dahdsr::Param, kBoxCount>& layout();

	visage::Bounds curveBounds() const;
	visage::Bounds boxBounds(size_t index) const;
	visage::Bounds chipBounds(size_t index) const;

	//-------//
	// Chips //
	//-------//

	/// THE SWITCHES, which are chips rather than boxes because a switch has no value to print and no
	/// range to drag — a value box for one would be a box whose number never changed.
	struct Chip {
		mod::dahdsr::Param param;
		const char* label;
	};
	/// TWO: Legato and Bipolar. It was five before the Mode collapse, three after it, four when Legato
	/// came back (see `chips()` for which of those two deletions was right), three again once
	/// Smoothing On was gone — the bottom of the Smoothing box is its off — and two since the Mode
	/// cycle became `choices()` (backlog GG).
	static constexpr size_t kChipCount = 4;
	static const std::array<Chip, kChipCount>& chips();

	/// **THE CHOICE: Mode, as a value box that opens a menu (backlog GG).** It was a chip that showed
	/// the current mode and advanced on a press, so reaching Once from Loop went THROUGH Free, applied
	/// — the multivalue toggle the UI rules out. The box opens its
	/// list from the descriptor; a pick is one value. It keeps the chip's cell, the last of three.
	static constexpr size_t kChoiceCount = 1;
	static const std::array<mod::dahdsr::Param, kChoiceCount>& choices();
	const ValueBox* choiceBoxForTest(size_t index) const {
		return index < kChoiceCount ? choiceBoxes_[index].get() : nullptr;
	}
	ValueBox* choiceBoxForTest(size_t index) {
		return index < kChoiceCount ? choiceBoxes_[index].get() : nullptr;
	}
	/// The cell a choice sits in; its box is inset one pixel so the cell's outline shows.
	visage::Bounds choiceBounds(size_t index) const;

	/// EVERY CONTROL A HAND CAN REACH THROUGH THIS CARD, in the device's own vocabulary. Same seam as
	/// `FilterCard::addressedParamsForTest` and `MsegCard`'s, and it exists because the reachability
	/// walk runs from the DECLARATIONS inward: it needs the card to say what it addresses, since a
	/// frame tree cannot see a gesture and the three shape controls are reached by a gesture alone.
	std::vector<mod::dahdsr::Param> addressedParamsForTest() const;

	/// Whether a chip is lit — its switch is on.
	bool chipOn(size_t index) const;
	/// WHETHER A CHIP CHANGES ANYTHING NOW — the honesty rule. Poly is dead in Free, which is one shape whatever
	/// the switch says (`mod::PolyShape::poly`): drawn dim and not pressable there.
	bool chipLive(size_t index) const;
	std::string chipLabel(size_t index) const;
	void toggleChip(size_t index);

	/// Move any caption whose MEANING follows a mode. See the definition: sustain becomes the loop's
	/// floor, and a caption that does not say so is describing the other mode.
	void followMode();

	/// What a box's caption currently reads. Exposed because followMode makes a claim — that the
	/// sustain box says it is a floor while the shape loops — and a claim about text is only
	/// checkable as text.
	const std::string& boxCaption(size_t index) const;

	/// One of the card's boxes, as a frame. Exposed for the reason ModState::watches is: "this box
	/// repaints when the armed flag moves" is a claim about a SUBSCRIPTION, and a case needs both
	/// halves of it to check.
	const visage::Frame* boxFrame(size_t index) const;

	/// THE CURVE THE CARD WOULD DRAW, as points in 0..1 across and IN THE ENVELOPE'S OWN RANGE up —
	/// which is 0..1 for an ordinary envelope and -1..1 for a bipolar one. Exposed for the reason the
	/// filter's curveGains is: "the picture is the envelope that plays" is a claim, and a claim about
	/// geometry has to be checkable against numbers rather than against a screenshot.
	/// BY REFERENCE: the cached curve, rebuilt only when the settings moved. A caller that wants it past the next call
	/// (one that may rebuild) copies it; `draw` and `marker` each read it once per paint and copy nothing.
	const std::vector<visage::Point>& curvePoints() const;

	/// ADVANCE THE PLAYHEAD, AND SAY WHETHER THE SCREEN IS OUT OF DATE. `MsegCard::update()` is the
	/// sibling and `OwlttGraph::update()` the original; this is the fourth instance of that contract.
	bool updateContent() override;

	/// WHERE THE PLAYHEAD IS DRAWN, in card coordinates.
	///
	/// **NOT DRAWN WHILE THE ENVELOPE LOOPS, AND THAT IS A DECLINE RATHER THAN A GAP.** A looping
	/// envelope's picture is TWO cycles — see `rebuildCurve`, which draws the cycle twice so the eye
	/// reads "this repeats" — so one position has two candidate x's and something has to choose.
	/// `stageEndX_` records the LAST occurrence, so a naive marker would always sit on the second
	/// cycle; that is defensible and nobody has chosen it. The choice is to be made off a screenshot
	/// rather than a guess, and until it is, this draws nothing rather than something arbitrary.
	struct Marker {
		visage::Point at { 0.0f, 0.0f };
		bool drawn = false;
	};
	[[nodiscard]] Marker marker() const;

	/// WHERE A VALUE SITS IN THE CURVE BOX, as 0 at the floor and 1 at the ceiling.
	///
	/// A pure function, and public, because it is where a bug lived that no assertion about
	/// curvePoints could have reached: the drawing clamped the envelope's output to 0..1, which for a
	/// bipolar envelope flattened every negative value onto the floor and put the rest in the wrong
	/// half of the box. The curve it was handed was correct throughout. If the claim is "this value
	/// draws here", the mapping has to be the thing under test.
	static float plotUnit(float value, bool bipolar) {
		const float floorValue = bipolar ? -1.0f : 0.0f;
		return std::clamp((value - floorValue) / (1.0f - floorValue), 0.0f, 1.0f);
	}

	/// Which stage each x position belongs to, so a case — and the drag — can say which segment of
	/// the drawn shape is under a coordinate.
	mod::dahdsr::Stage stageAtX(float x) const;

	/// Where a stage's segment ENDS, in 0..1 across. The other half of stageAtX, and what a case uses
	/// to say that a stage got wider — which is the whole claim of this card's x axis.
	float stageEndX(mod::dahdsr::Stage stage) const;

	//------------------------------------------------------------------//
	// The bend — the MSEG's curve gesture on this card (backlog MN)     //
	//------------------------------------------------------------------//

	/// WHAT A BEND CAUGHT: the MSEG's `mseg::CurveTarget` in this card's vocabulary: per-segment
	/// curvature for the DAHDSR, with the MSEG's gesture. The three curves and a plain drag on them existed
	/// already; what the MSEG had and this did not is the rest of its gesture — Opt on a breakpoint bending both
	/// sides, the highlight of what was caught, and one undo step per bend.
	///
	/// A `Stage` target is one curved stage (Attack, Decay or Release). A `Peak` is the top of the shape, the
	/// breakpoint between Attack and Decay (either end of a Hold plateau counts), caught with Opt: it bends both.
	struct BendTarget {
		enum class Kind { None, Stage, Peak };
		Kind kind = Kind::None;
		/// The curved stages this bend moves, as a mask of `bit(stage)` — what is bent and what is lit.
		uint8_t stages = 0;

		static constexpr uint8_t bit(mod::dahdsr::Stage stage) {
			return static_cast<uint8_t>(1u << static_cast<unsigned>(stage));
		}
		[[nodiscard]] bool bends(mod::dahdsr::Stage stage) const { return (stages & bit(stage)) != 0; }
		friend bool operator==(const BendTarget&, const BendTarget&) = default;
	};

	/// WHAT A PRESS AT `point` (card coordinates) WOULD BEND. Plain: the curved stage under it, or nothing over a
	/// flat one (delay, hold, sustain have no curve, and bending a neighbour instead would be a silent edit). Opt:
	/// the peak if the point is within `kBendGrabPixels` of it — the MSEG's grab radius, and the peak wins ties
	/// as the MSEG's waypoint does — else the same as plain. The DAHDSR has no handles for a plain drag to move,
	/// so unlike the MSEG a plain drag still bends.
	[[nodiscard]] BendTarget bendTargetAt(visage::Point point, bool opt) const;

	/// BEND WHAT THE TARGET NAMES by `delta`, clamped to the shape range. A PEAK BENDS BOTH SIDES THE SAME WAY
	/// IN THIS DEVICE'S NUMBERS, and that IS the MSEG's inverse pair: the MSEG's numbers bend toward a segment's
	/// destination, so its waypoint takes +delta before and -delta after to round a peak; the DAHDSR reads its
	/// falling stages backwards (`kShapeMax`: positive is fuller on every stage), so the same rounded peak — the
	/// curve rising off both sides with the pointer — is +delta on Attack and on Decay.
	static mod::dahdsr::Settings withBend(const mod::dahdsr::Settings& settings, const BendTarget& target,
	                                      float delta);

	/// What is lit: the bend in progress, else the Opt-hover. The MSEG's `highlightForTest`.
	[[nodiscard]] BendTarget highlightForTest() const;

	/// The MSEG's numbers (mseg_card.cpp `kGrabPixels`, `kCurveTravelForFullBend`): a full bend is half the
	/// curve box's height of travel.
	static constexpr float kBendGrabPixels = 13.0f;
	static constexpr float kBendTravelForFullBend = 0.5f;

	static constexpr float kPadX = 9.0f;
	static constexpr float kHeadHeight = 24.0f;
	/// The mock's `--preview-h`, spent on the shape editor.
	static constexpr float kCurveHeight = 44.0f;
	static constexpr float kRowGap = 4.0f;
	static constexpr float kChipHeight = 16.0f;
	static constexpr float kGap = 6.0f;
	/// The Trig pad's column at the shape's left — the pad and a gap.
	static constexpr float kTrigColumn = TrigPad::kDiameter + 6.0f;

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST, into
	/// lists that are still alive.
	std::vector<Watch> watches_;
	void rebuildCurve();
	void bindBox(size_t index);

	backplane::DeviceHost& host_;

	/// THIS CARD'S SETTINGS, ASSEMBLED FROM THE DEVICE'S OWN FIELD ROW.
	///
	/// Replaces `bridge_.modRack().dahdsrAt(device_)`. The device decodes its own row — `settingsOf`
	/// puts ABSENT fields at their declared defaults rather than at zero, which is `device_row.h`'s
	/// rule and the reason this is not a memcpy.
	mod::dahdsr::Settings settings() const;

	/// WRITE THE WHOLE ROW BACK. Replaces `bridge_.setEnvelopeSettings(device_, s)`.
	void writeSettings(const mod::dahdsr::Settings& settings);

	/// WHERE THE ENVELOPE IS, OR IDLE IF NOBODY PUBLISHES IT. See `DeviceHost::playheadAt`: a
	/// playhead is a READING, so a host without a publisher declines and this answers idle — the
	/// marker is then not drawn at all rather than frozen at stage zero.
	mod::Playhead playhead() const;
	DeviceInstanceId device_ = kNoInstance;
	ModState& mod_;
	EditHistory& history_;

	std::array<std::unique_ptr<DeviceControl>, kBoxCount> controls_;
	/// **THE ENABLE SQUARE'S OWN CONTROL, WHICH THIS CARD DID NOT HAVE (BM).** `controls_` covers
	/// only the visible boxes, so the head's switch was toggled by writing the field straight
	/// through the host — which skips `DeviceControl`, and `DeviceControl` is where the undo step
	/// is recorded. Turning a modulator off was therefore not undoable while turning an audio
	/// effect off was. Built lazily because a device with no declared enable has none.
	mutable std::unique_ptr<DeviceControl> enableControl_;

public:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt.
	device::Modulatable* enableModulatable() override;
	/// The hand-drawn parts' tooltips — see `FxCard::contentTooltipAt`.
	const char* contentTooltipAt(visage::Point local) const override;

private:
	std::array<std::unique_ptr<ValueBox>, kBoxCount> boxes_;
	std::array<std::unique_ptr<DeviceControl>, kChoiceCount> choiceControls_;
	std::array<std::unique_ptr<ValueBox>, kChoiceCount> choiceBoxes_;
	/// ONE ROW OF `kChipCount + kChoiceCount` EQUAL CELLS — the chips first, then the choice.
	visage::Bounds cellBounds(size_t cell) const;

	/// The shape, cached. Rebuilt when the settings move rather than per frame — running an envelope
	/// is cheap but it is not free, and a card repaints far more often than its settings change.
	std::vector<visage::Point> curve_;
	/// Where each stage's segment ends, in 0..1 across (the last occurrence) — the cases' and the marker's.
	std::array<float, 6> stageEndX_ {};

	/// WHAT PLAYHEAD WAS LAST DRAWN, so `update()` can tell a moving envelope from a still one.
	///
	/// **A PLAIN VALUE, WATCHING NOTHING.** It is recorded here that this adds no subscription: the
	/// marker is INK, not a widget, so there is nothing for `~EnvCard` to unwatch. That destructor
	/// takes `this` and every box off `ModState`'s raw-pointer list and is the only place that can —
	/// a box does not know it is watched. Anything added to this card that DOES watch must be
	/// unwatched there, and the merge that forgets it will be textually clean.
	mod::Playhead drawnPlayhead_ {};
	/// Which mode the captions are currently written for. A tri-state would be tidier than a bool
	/// plus the constructor's implicit false; it is not worth it for one caption.
	bool labelledForLoop_ = false;
	mod::dahdsr::Settings curveFrom_ {};
	size_t curveRebuilds_ = 0;
	bool haveCurve_ = false;

	/// The envelope used to DRAW, distinct from the four the engine plays. Constructed once.
	std::unique_ptr<mod::dahdsr::Envelope> preview_;
	std::unique_ptr<DeviceControl> trigControl_;
	/// "NOTES FIRE TRIG" (backlog NU), the pad's normal — declared before the pad, which holds it.
	std::unique_ptr<DeviceControl> notesTrigControl_;
	std::unique_ptr<TrigPad> trigPad_;

	/// EVERY DRAWN SEGMENT'S STAGE AND SPAN, in 0..1 across, in drawing order (backlog MN). `stageEndX_` keeps
	/// one edge per STAGE, the LAST occurrence, so on a looping shape (two cycles) the drag read the first cycle
	/// by the SECOND cycle's edges: everything left of the second attack's end bent the Attack, the first decay
	/// included. The bend asks this list instead, so either cycle catches its own stage.
	struct Span {
		mod::dahdsr::Stage stage = mod::dahdsr::Stage::Idle;
		float from = 0.0f;
		float to = 0.0f;
	};
	std::vector<Span> spans_;

	/// What the bend in progress caught, the settings it started from, and where the press was. ABSOLUTE FROM
	/// ITS ANCHOR, the MSEG's rule (`MsegCard::curveBefore_`): every move recomputes from the anchor, so the
	/// bend is a function of where the pointer IS, not of how many events reached it.
	BendTarget bend_ {};
	mod::dahdsr::Settings bendBefore_ {};
	float bendAnchorY_ = 0.0f;
	/// What an Opt-hover found, so the highlight shows before a button goes down (the MSEG's `hoverTarget_`).
	BendTarget hover_ {};
};

} // namespace feathers::ui
