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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/shaper_expanded.h"
#include "plugin/ui/shaper_colour_cell.h"
#include "dsp/slice_build.h" // fieldWithOverride — the hysteresis line prints what the engine hears
#include "plugin/ui/gamma_box.h"

#include "fx/shaper/shaper_curve.h"
#include "plugin/ui/shaper_plot.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/zone_coord_cell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace feathers::ui {

namespace shp = fx::shaper;

namespace {

/// THE KNOB ROW, IN ORDER. The card's four first, in the card's order, so a hand that learned them
/// there finds them in the same places here — then the three the card does not carry.
///
/// HYSTERESIS SITS LAST BECAUSE IT IS A BIAS ON A FIELD RATHER THAN A COORDINATE IN ONE. Knee,
/// Colour and Gamma say where you are; Drive and Mix say what is done with it; Emphasis tilts which
/// frequencies the curve bites. This one says how much of what the position already decided you want to hear, which
/// is a different kind of statement and belongs at the end of the row rather than among the axes.
/// **WHAT ONE DRAG OF AN UNBOUNDED FIELD CONTROL IS WORTH, in the control's own units.**
///
/// One unit — which is what Gamma and Rotate each had while they were declared 0..1, because a
/// `ValueBox` with no explicit span drags its whole declared range. Widening those declarations to
/// remove their walls would otherwise have released the span to `GammaBox`'s sixteen laps and made
/// both controls sixteen times faster than the ones already in use. **The wall and the
/// sensitivity were the same number, and only the wall was meant to go.**
constexpr double kShaperFieldDragSpan = 1.0;

constexpr std::array<shp::Param, ShaperExpanded::kKnobCells> kPanelKnobs {
	shp::Param::Knee,  shp::Param::Colour,    shp::Param::Gamma,      shp::Param::Drive,
	shp::Param::Threshold, shp::Param::Emphasis, shp::Param::Hysteresis,
	// **THE INPUT FILTERS, AT THE END OF THE TOP ROW (backlog FA)** — the card carries them under Knee and
	// Colour, where its second row had room; here the second row is full, so they take two new columns.
	shp::Param::InputHpf, shp::Param::InputLpf,
	// **ROTATE IS NOT IN THIS ROW ANY MORE — it sits INSIDE the terrain box** (backlog IJ). It turns
	// the plane the pad travels in, so
	// it belongs on the picture it turns. See `rotate_`, created after the pad so it draws over it.
	// **THE SECOND ROW, AND ITS ONLY TENANT — beneath Threshold (once Mix), by design.** The crossfade's width
	// belongs under the control that decides where the crossfade starts; see `Param::Aggression` for
	// why it is a knob of its own rather than back on Knee where the firmware had it.
	shp::Param::Aggression,
	// AND THE TRIM, BESIDE IT UNDER DRIVE — the second row is the level row. See `Param::Trim`.
	shp::Param::Trim,
	// AND THE FIVE OF BACKLOG IE — see `kSecondRowColumns` for where each sits and why.
	shp::Param::Cheby, shp::Param::Stair, shp::Param::Bloom, shp::Param::Sag, shp::Param::Loop,
	// **AND THE CROSSFADE MIX, LAST AND IN THE CORNER (backlog FA)** — it is the last thing the device does.
	shp::Param::DryWet,
};


/// THE DEVICE'S, NOT THIS FILE'S. It was written here and open-coded again in `shaper_card.cpp`;
/// both now call `fx::shaper::shortLabelOf`, which lives beside the prefix it strips.
using shp::shortLabelOf;

/// WHAT THE SUB'S INTERVAL ACTUALLY IS. The ratio is a count of zero crossings per sign flip, which
/// is a number nobody hears; the INTERVAL is what they hear. Named from the firmware's own comment on
/// the field (`2=octave, 3=twelfth, 4=2oct, 5=2oct+3rd, 6=2oct+5th`).
const char* subIntervalName(int32_t ratio) {
	switch (ratio) {
	case 2: return "octave";
	case 3: return "twelfth";
	case 4: return "2 oct";
	case 5: return "2 oct + 3rd";
	case 6: return "2 oct + 5th";
	default: return "-";
	}
}

std::string twoPlaces(float v) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(v));
	return buffer;
}

} // namespace

ShaperExpanded::ShaperExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — see generic_card.cpp. The parameter stays
	// until the signature pass; discarded so a deferred cleanup is not read as a forgotten use.
	(void)registry;
	controls_.resize(static_cast<size_t>(shp::Param::kCount));

	for (const shp::Param p : kPanelKnobs) {
		if (p == shp::Param::Colour) {
			// **COLOUR'S MODE CELL (backlog IW)** — the card's, from the same builder: Inflate or Free in the dropdown,
			// and the live mode's value in the box.
			colourCell_ = makeColourModeCell([this](shp::Param q) -> device::Modulatable& { return control(q); },
			                                 kKnobDiameter);
			addChild(colourCell_.get());
			cells_.push_back(colourCell_.get());
			// ALL THREE IN THE REACHABILITY LIST rather than `knobParams_`, which stays 1:1 with `knobs_`.
			extraReached_.push_back(shp::Param::Colour);
			extraReached_.push_back(shp::Param::ColourMode);
			extraReached_.push_back(shp::Param::InflateAmount);
			continue;
		}
		if (p == shp::Param::Gamma) {
			// **A BOX, NEVER A KNOB**, by design; this surface was found nonconforming after the same
			// defect was fixed on automod's.
			// The seating and the drag span live in `gamma_box.h`; this row supplies only the caption,
			// because it captions each cell under the widget.
			auto box = std::make_unique<GammaBox>(control(p), shortLabelOf(p));
			// **ONE UNIT PER DRAG — RESTORING THE FEEL THE 0..1 DECLARATION GAVE, now that the wall
			// behind it is gone.**
			//
			// The defect, after the range was widened: with gamma driven a long way, a tiny bump on fine
			// colour barely changed the transfer, as if gamma were ignored. That
			// was this line's fault, not the field's. `GammaBox` sets `min(sixteen laps, the declared
			// range)`; while the range was 1.0 that clamped to ONE unit per drag, and widening the
			// range silently released it to SIXTEEN. The same gesture began covering sixteen times the
			// ground, which is how a control stops feeling like a control.
			//
			// **AND THE FIELD SATURATES IN GAMMA, so the extra travel bought almost nothing.**
			// MEASURED at colour 0.38, counting curves more than 0.15 apart: 0..1 holds 351 distinct
			// curves, 0..4 holds 679, 0..16 holds 758 and 0..64 holds 763. Sixteen units per drag
			// spends most of its travel re-treading ground while making every pixel jump five distinct
			// curves — which is exactly the "nothing I do matters" that was reported.
			//
			// **IT ALSO PUTS THE PRECISION CLIFF OUT OF REACH OF A HAND.** `curveFor` sums gamma and
			// colourPhase in float32, so a large gamma swallows a small phase bump outright: MEASURED,
			// at gamma 262144 a 0.01 bump changes the curve by exactly 0.000000. At one unit per drag
			// that is 262,144 drags away; at sixteen it was 16,384. Neither is likely, but the fix for
			// it is a real wrap and this is not that — it is only distance.
			box->setDragSpan(kShaperFieldDragSpan);
			addChild(box.get());
			cells_.push_back(box.get());
			gamma_ = std::move(box);
			extraReached_.push_back(p);
			continue;
		}
		auto knob = std::make_unique<Knob>(control(p), shortLabelOf(p), notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		knobParams_.push_back(p);
	}

	// THE PAD IS A CHILD FRAME, so it consumes its own presses and this panel's hit test never has to
	// know where it is. Waypoint's card draws its plane itself and pays for it with a gate against the
	// card-drag; a child frame has no such problem.
	// **THE BLOB AND THE ROW, exactly as automod's pad takes them.** The pad writes folded travel by
	// INDEX and reads the device's fields by index; it never names a control, which is what lets the
	// same shape serve a device with eight phases and one with thirty-nine.
	pad_ = std::make_unique<ShaperPad>(
	    [this](size_t index, float value) {
		    host_.setField(device_, shp::kTravelFirst + index, value);
	    },
	    [this](size_t field) { return host_.fieldAt(device_, field); }, control(shp::Param::Rotate));
	addChild(pad_.get());
	// ONE DRAG OF THE PAD IS ONE UNDO STEP (backlog JB): the whole folded travel, read at the press and recorded at
	// the release, by device id.
	pad_->onHoldBegan = [this] { padGesture_.begin(host_, device_, shp::kTravelFirst, ShaperPad::kPhaseCount); };
	pad_->onHoldEnded = [this] { (void)padGesture_.end(host_, history_, "Pad"); };

	// **ROTATE, INSIDE THE TERRAIN BOX** (backlog IJ) — a compact box in the pad's top-right corner, made
	// AFTER the pad so it draws over the map it turns.
	//
	// **A BOX FOR THE SAME REASON GAMMA IS ONE: the quantity is unbounded.** `Param::Rotate` is declared as
	// a symmetric drag window rather than a domain, and a knob maps one drag across the WHOLE declared
	// range — an unusably coarse dial. **ONE UNIT PER DRAG**, the sensitivity this control already had:
	// `GammaBox` defaults to sixteen laps, and MEASURED at 5-degree resolution one unit of rotate covers
	// 24 distinct planes and sixteen covers 412.
	rotate_ = std::make_unique<GammaBox>(control(shp::Param::Rotate), shortLabelOf(shp::Param::Rotate));
	rotate_->setDragSpan(kShaperFieldDragSpan);
	addChild(rotate_.get());
	extraReached_.push_back(shp::Param::Rotate);

	// THE ENABLE SQUARE AND ITS "Active" CAPTION THAT HEADED THIS BODY ARE THE WINDOW HEADER'S CHIP NOW
	// (2026-09-26), one for every device; the row they took is given back to the pad.

	// THE SIX EXTRAS ARE FADER PILLS (backlog IK) — each an amount AND its power, with the knob's whole
	// modulation surface. Children with their own events, so this surface places them and nothing more;
	// see `FaderPill`.
	for (size_t i = 0; i < kExtraCount; ++i) {
		auto pill = std::make_unique<FaderPill>(control(kExtraParams[i]), control(shp::kExtraPowers[i]),
		                                        shortLabelOf(kExtraParams[i]));
		pill->setAnnouncer([this](const std::string& line) {
			if (notifier_ != nullptr) {
				notifier_->say(line);
			}
		});
		addChild(pill.get());
		pills_.push_back(std::move(pill));
	}
	poly_ = std::make_unique<SwitchChip>(control(shp::Param::Poly), "Poly");
	addChild(poly_.get());
}

DeviceControl& ShaperExpanded::control(shp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] =
		    std::make_unique<DeviceControl>(host_, device_, shp::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}


void ShaperExpanded::showInSignal(bool live) {
	// THE POPOUT SAYS THE SAME THING AS THE CARD, and it has to say it in its own words because the
	// two hold different widget lists — but the RULE is one rule, spelled out at
	// `ShaperCard::showInSignal`: at Knee 0 the device consults no table, so everything but Knee is
	// unreachable, and Knee stays lit because it is the way back out.
	//
	// **BOTH SURFACES OR NEITHER.** The card and the popout show the same device, and a Drive that
	// greys on one and not the other is worse than one that greys nowhere — it makes the player
	// distrust the greying itself, which is the signal we are trying to add.
	for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
		// TRIM AND THE INPUT FILTERS STAY LIT TOO (backlog FA): they run ahead of the bypass, so at Knee 0 they
		// still change the sound — see `shp::actsAtKneeZero`.
		knobs_[i]->setActive(live || shp::actsAtKneeZero(knobParams_[i]));
	}
	for (auto& coord : coords_) {
		coord->setEnabled(live);
	}
	// COLOUR'S MODE CELL GREYS WITH THE REST (backlog IW) — the coordinate box it replaced did.
	if (colourCell_ != nullptr) {
		colourCell_->setEnabled(live);
	}
	for (auto& pill : pills_) {
		pill->setActive(live);
	}
	inSignal_ = live;
}
shp::Settings ShaperExpanded::settings() const {
	// THE ROW, READ THROUGH THE SEAM — the same change as the card's `settings()`, for the same reason:
	// `fxChain()` reaches a `DeviceRow` through a type only the parameter road has, and
	// `fieldsPresentAt`/`fieldAt` are that read in the vocabulary both hosts share. It is what lets the
	// RACK open this panel at all.
	shp::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	shp::Settings s = shp::stateOf(row, present);
	// **AND THE FOLDED TRAVEL, WHICH `stateOf` CANNOT SEE.** It takes the params-only `Row` by design,
	// so the eight floats past the params are invisible to it — and a surface that could not see them
	// drew the curve the coordinates alone imply while the device played a different one. That is the
	// same shape as the drive meter freezing and as `sameCurveAs` ignoring travel: a reading that does
	// not reach the surface. Read through the same seam, so the rack gets it too.
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		const size_t at = shp::kTravelFirst + i;
		s.travel[i] = at < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, at) : 0.0f;
	}
	return s;
}

void ShaperExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the pad's two controls and the six
	// switches are edited by gestures rather than by widgets. HARM shipped without this line and could
	// not be modulated at all; Waypoint's panel was never handed the state and every control on it
	// reported itself un-armable.
	for (size_t i = 0; i < static_cast<size_t>(shp::Param::kCount); ++i) {
		control(static_cast<shp::Param>(i)).setModulation(mod, history);
	}
}

visage::Bounds ShaperExpanded::padBounds() const {
	return {kPad, kPad, kPadWidth, kPadHeight};
}

visage::Bounds ShaperExpanded::plotBounds() const {
	const visage::Bounds pad = padBounds();
	return {pad.right() + kRowGap * 2.0f, pad.y(), kPlotWidth, kPlotHeight};
}

visage::Bounds ShaperExpanded::readoutBounds() const {
	const visage::Bounds plot = plotBounds();
	return {plot.x(), plot.bottom() + kRowGap, kPlotWidth,
	        std::max(padBounds().bottom() - plot.bottom() - kRowGap, 0.0f)};
}

visage::Bounds ShaperExpanded::extraBounds(size_t i) const {
	if (i >= kExtraCount) {
		return {};
	}
	// **UNDER BOTH KNOB ROWS, NOT ONE.** On the installed build the window's Aggression knob lay on
	// top of the extras buttons. It did, exactly: this read
	// `padBounds().bottom() + kRowGap + column + kRowGap`, which is the precise y the SECOND knob row
	// is laid at, so the two occupied the same band. The card's own `extraBounds` was moved down by
	// `secondRowHeight()` when that row was added and this one was not — the same derived top written
	// in two files, corrected in one.
	//
	// `minimumSize` already reserves the space: its sum carries two `columnHeight` terms and the
	// switch row after them, so this is the layout catching up with a height that was already right.
	const float top = padBounds().bottom() + kRowGap + Knob::columnHeight(kKnobDiameter) + kRowGap
	                  + Knob::columnHeight(kKnobDiameter) + kRowGap;
	return {kPad + static_cast<float>(i) * (kExtraWidth + kExtraGap), top, kExtraWidth,
	        kSwitchHeight};
}

visage::Bounds ShaperExpanded::polyBounds() const {
	const visage::Bounds last = extraBounds(kExtraCount - 1);
	return {last.right() + kExtraGap, last.y() + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kPolyWidth,
	        SwitchChip::kHeight};
}

visage::Point ShaperExpanded::maximumSize() const {
	const visage::Point natural = minimumSize();
	return { natural.x + kPad, natural.y + kPad };
}

visage::Point ShaperExpanded::minimumSize() const {
	// STATED AS THE ROWS THIS PANEL ACTUALLY LAYS OUT rather than measured after the fact, because the
	// overlay CLAMPS rather than scrolling — a panel asking for more than it fills leaves blank canvas
	// and one asking for less has its bottom row silently cut off. Waypoint's panel says the same.
	const float width = kPad + kPadWidth + kRowGap * 2.0f + kPlotWidth + kPad;
	// **BOTH KNOB ROWS.** The second one — Aggression, beneath Mix — is a full knob column and a gap,
	// and leaving it out of this sum is exactly the failure this function's own comment describes: the
	// overlay clamps rather than scrolling, so the bottom row is silently cut off. The panel's layout
	// case caught it here rather than in a screenshot, which is what that case is for.
	const float height = kPad + kPadHeight + kRowGap
	                     + Knob::columnHeight(kKnobDiameter) + kRowGap
	                     + Knob::columnHeight(kKnobDiameter) + kRowGap + kSwitchHeight + kPad;
	// AND WIDE ENOUGH FOR THE ROWS UNDERNEATH, which is a different number from the two pictures
	// above them. Whichever is larger wins, so a row growing cannot silently overflow.
	const float knobRow = kPad + static_cast<float>(kTopRowCells) * kCellWidth + kPad;
	const float extraRow =
	    kPad + static_cast<float>(kExtraCount) * (kExtraWidth + kExtraGap) + kPolyWidth + kPad;
	return {std::max({width, knobRow, extraRow}), height};
}

std::string ShaperExpanded::title() const {
	// THE DEVICE'S OWN NAME, asked of the descriptor rather than spelled here, so the panel and the
	// card's head cannot come to say different things.
	return shp::kTraits.name;
}

void ShaperExpanded::resized() { layOut(); }

bool ShaperExpanded::updateContent() {
	// THE CARD'S `update()`, LINE FOR LINE, and the two rules it carries: exact equality, because the
	// question is whether the screen shows THESE settings, which is bitwise; and nothing while
	// invisible, because `redraw()` is refused for a frame that is not being drawn and a `true` here
	// is counted as a frame ASKED FOR — a count that must not include frames nobody could paint.
	//
	// AND IT READS, NEVER BUILDS: `settings()` goes through `host_.fieldAt`, not through `control()`,
	// which lazily constructs a `DeviceControl` — the trap `HarmCard::update` fell into (bbba474).
	if (!isVisible()) {
		return false;
	}
	const shp::Settings now = settings();
	// COLOUR'S MODE CELL SHOWS THE LIVE MODE'S VALUE (backlog IW) — a mode change swaps which box it shows.
	const bool modeSwapped = colourCell_ != nullptr && colourCell_->refresh();
	// **AND THE DRIVE METER, WHICH IS NOT PART OF `Settings` AND SO WAS NOT WATCHED AT ALL.**
	//
	// THE DEFECT: the meter froze unless a control changed. Exactly that: this tick returned early whenever the SETTINGS
	// were unchanged, and a meter is not a setting — it is a reading the engine produces. So the card
	// redrew only when a parameter happened to force one, and the meter froze at whatever it held.
	//
	// **A READING NEEDS ITS OWN DIRTY TEST BECAUSE IT IS NOT STATE.** `Settings` is what a patch
	// stores; the peak is what the engine is doing this instant. `drawnSettings_` can never see it,
	// and any future reading drawn on this card — a playhead, a gain reduction — lands in the same
	// hole unless it is added here too.
	//
	// THE EPSILON IS A PIXEL, NOT A TOLERANCE: the meter colours a curve about 200 px wide, so a
	// change under 1/400 of full scale cannot move the boundary and a redraw for it would be work
	// nobody can see. It is deliberately far below the fall rate, so a falling meter redraws every
	// tick — which is what a falling meter is supposed to do.
	float drive = 0.0f;
	const bool metered = host_.drivePeakAt(device_, drive);
	const bool driveMoved = metered != haveDrive_ || std::fabs(drive - drawnDrive_) > 0.0025f;
	if (haveDrawn_ && now == drawnSettings_ && !driveMoved && !modeSwapped) {
		return false;
	}
	drawnDrive_ = drive;
	haveDrive_ = metered;
	drawnSettings_ = now;
	// THE SAME HOOK THE CARD USES, and for the same reason: this is the one place that knows the
	// settings moved. See `ShaperCard::update`.
	showInSignal(!now.linear());
	haveDrawn_ = true;
	redraw();
	return true;
}

void ShaperExpanded::layOut() {
	pad_->setBounds(padBounds());
	// ROTATE IN THE TERRAIN BOX'S TOP-RIGHT CORNER, inset so the map's border stays visible around it.
	{
		const visage::Bounds pad = padBounds();
		constexpr float kInset = 6.0f;
		constexpr float kRotateWidth = 118.0f;
		rotate_->setBounds(pad.right() - kRotateWidth - kInset, pad.y() + kInset, kRotateWidth,
		                   rotate_->naturalHeight());
	}
	const float top = padBounds().bottom() + kRowGap;
	// A PLAIN KNOB COLUMN AGAIN. The coordinate box IS one, row for row, so the row no longer needs
	// to be sized for a taller widget in it.
	const float column = Knob::columnHeight(kKnobDiameter);
	const float cell = std::max(kCellWidth, (width() - 2.0f * kPad) / static_cast<float>(kTopRowCells));
	// **`cells_`, NOT `knobs_` — THE BOUND THAT LEFT A WIDGET AT ZERO SIZE.** This loop kept its old
	// bound after the body started indexing `cells_`, so it placed four of the five cells and left
	// the coordinate box at 0x0. It drew nothing and reported nothing; the layout case caught it by
	// asking every placed child for a positive width.
	for (size_t i = 0; i < cells_.size(); ++i) {
		// **THE SECOND ROW IS A COLUMN AND A GAP LOWER, AT MIX'S x.** Derived from the same `cell` the
		// top row uses, so the two rows cannot drift apart if the panel is resized.
		// **THE SECOND ROW IS A COLUMN AND A GAP LOWER, EACH CELL UNDER THE ONE IT BELONGS TO** —
		// Aggression under Mix, Trim under Drive, in the order `kPanelKnobs` declares them. Derived
		// from the same `cell` the top row uses, so the rows cannot drift apart on a resize.
		const bool second = i >= kTopRowCells;
		static_assert(kSecondRowColumns.size() == kKnobCells - kTopRowCells);
		const size_t which = second ? i - kTopRowCells : 0;
		const size_t columnOf = kSecondRowColumns[which];
		const float slot = second ? static_cast<float>(columnOf) : static_cast<float>(i);
		const float rowTop = second ? top + column + kRowGap : top;
		const visage::Bounds box { kPad + cell * slot, rowTop, cell, column };
		// GAMMA'S BOX IS CENTRED IN THE SPACE A KNOB'S FACE WOULD FILL — `GammaBox::cellBounds`, the
		// same arithmetic automod's two surfaces use.
		if (cells_[i] == gamma_.get()) {
			cells_[i]->setBounds(GammaBox::cellBounds(box, kKnobDiameter));
			continue;
		}
		cells_[i]->setBounds(box);
	}
	// THE SIX FADER PILLS, where the pills were drawn before they were widgets.
	for (size_t i = 0; i < pills_.size(); ++i) {
		pills_[i]->setBounds(extraBounds(i));
	}
	poly_->setBounds(polyBounds());
}

std::vector<std::string> ShaperExpanded::readoutLinesForTest() const {
	const shp::Settings s = settings();
	// **`curveOf`, THE ONE CALL THE ENGINE AND THE PLOT BOTH MAKE — NOT `deriveCurve` OF THE COORDINATES.**
	// THE DEFECT: the small readouts beside the extras never changed.
	// They could not: the pad commits TRAVEL (the eight phases past the params), never Colour or Gamma,
	// and this called `deriveCurve(knee, colour, gamma, ...)` with no travel — so a drag moved the sound
	// and the curve beside it while these read the coordinates' answer and sat still. The same shape
	// `settings()` records for the curve, one widget over.
	const shp::CurveParams p = shp::curveOf(s);
	std::vector<std::string> lines;
	lines.push_back(std::string("Sub  ") + subIntervalName(p.subRatio) + "  x"
	                + twoPlaces(p.subIntensity));
	lines.push_back("Comb  " + std::to_string(p.stride) + " smp  x" + twoPlaces(p.feedback));
	lines.push_back("Rotate  " + std::to_string(p.rotation) + " bits");
	// WHAT THE TWO SLEW STAGES HEAR, the larger of them — each scales the one slew field by its own knob.
	lines.push_back("Slew  x" + twoPlaces(std::max(p.lowpassAmount, p.integratorAmount)));
	// **WHAT IS HEARD, NOT WHAT THE FIELD OFFERS** — the knob scales the field by the engine's own law
	// (`fieldWithOverride`, see `Shaper::beginBlock`), and since backlog IA it defaults to 0, which
	// removes it: this printed the field's -1.00 at full ink while nothing of it was in the sound.
	const float heardHysteresis
	    = std::copysign(fieldWithOverride(std::fabs(p.hysteresis), s.hysteresis), p.hysteresis);
	lines.push_back("Hysteresis  "
	                + twoPlaces(heardHysteresis == 0.0f ? 0.0f : heardHysteresis)); // no "-0.00" for a scaled-away negative
	lines.push_back("Asymmetry  " + twoPlaces(p.asymmetry));
	return lines;
}

void ShaperExpanded::draw(visage::Canvas& canvas) {
	++draws_;
	const shp::Settings s = settings();

	// THE CURVE, the same drawing the card makes, at this panel's size.
	// **HOW HARD THE TABLE IS ACTUALLY BEING DRIVEN**, asked of the host rather than computed here —
	// it is a READING the engine produces, and a card cannot derive it from the field row. The host
	// declines when nobody publishes, and `drawShaperCurve` then draws one colour: an unmetered device
	// must not read as an unreached one.
	float drive = 0.0f;
	if (!host_.drivePeakAt(device_, drive)) {
		drive = 0.0f;
	}
	drawShaperCurve(canvas, plotBounds(), s, false, drive);
	canvas.setColor(withAlpha(kMuted, 0.8f));
	canvas.text("Transfer curve", font(kKnobLabelSize), visage::Font::kLeft, plotBounds().x(),
	            plotBounds().y() - 13.0f, kPlotWidth, 12.0f);
	canvas.text("Colour x Gamma", font(kKnobLabelSize), visage::Font::kLeft, padBounds().x(),
	            padBounds().y() - 13.0f, kPadWidth, 12.0f);

	// WHAT THE FIELD IS SAYING HERE — the readout that stands in for nine knobs this device
	// deliberately does not have. Each line is greyed when its scaler is at zero, so the panel says both
	// "the field asks for this" and "you are not hearing it", which are different facts and a player
	// needs both: a scaler that seems to do nothing is usually a field asking for nothing HERE.
	const visage::Bounds box = readoutBounds();
	const std::vector<std::string> lines = readoutLinesForTest();
	// LIVE IFF ITS SCALER IS UP (backlog II) — the lines already print what is HEARD (`curveOf` scaled
	// them), and the greying says which of them the knobs have taken to zero.
	const std::array<bool, 6> live {s.subOn && s.sub > 0.0f, s.feedbackOn && s.feedback > 0.0f, s.mangleOn && s.mangle > 0.0f,
	                                (s.lowpassOn && s.lowpass > 0.0f) || (s.integratorOn && s.integrator > 0.0f),
	                                s.hysteresis > 0.0f, true};
	const float lineHeight = 14.0f;
	for (size_t i = 0; i < lines.size(); ++i) {
		const float y = box.y() + static_cast<float>(i) * lineHeight;
		if (y + lineHeight > box.bottom()) {
			break;
		}
		canvas.setColor(live[i] ? withAlpha(kInk, 0.9f) : withAlpha(kDisabled, 0.75f));
		canvas.text(lines[i], font(kKnobLabelSize), visage::Font::kLeft, box.x(), y, box.width(),
		            lineHeight);
	}


	// THE APOLOGY THAT USED TO BE HERE IS GONE, AND THAT IS THE FIX RATHER THAN A DELETION. This drew
	// "Integrator overrides Lowpass" whenever both were on, because the firmware gives the integrator
	// precedence — so one labelled switch silently disabled another and the panel had to explain it.
	// The two stages have their own state now and both run; `Shaper::beginBlock` says why that
	// divergence is the honest way round. A surface that has to caption a control's misbehaviour is a
	// surface reporting a bug it could have fixed.
}

std::vector<shp::Param> ShaperExpanded::addressedParamsForTest() const {
	// ASKED OF THE WIDGETS AND TABLES THIS PANEL ACTUALLY BUILT, never restated by hand.
	std::vector<shp::Param> out = knobParams_;
	// AND WHAT A KNOB REACHES BESIDES ITS OWN CONTROL — Colour's phase box, recorded where it was
	// built. See `extraReached_`.
	out.insert(out.end(), extraReached_.begin(), extraReached_.end());
	// (NOT `Enabled`: the window header's chip reaches it, for every panel.)
	for (const shp::Param p : kExtraParams) {
		out.push_back(p);
	}
	// AND EACH PILL'S POWER, which the same pill reaches by its click (backlog IK).
	for (const shp::Param p : shp::kExtraPowers) {
		out.push_back(p);
	}
	// AND POLY, the chip after the extras (backlog OM).
	out.push_back(shp::Param::Poly);
	// The pad edits Colour and Gamma, both of which the knob row already carries — listed once, which
	// is what the reachability guard wants: it asks whether a control is reachable, not how often.
	return out;
}

} // namespace feathers::ui
