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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/automod_card.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/automod_coords.h"

#include "fx/automod/automod.h"
#include "plugin/ui/owltt_graph.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace amd = feathers::fx::automod;
/// THE AUDIBLE SPAN IS BORROWED FROM THE MULTIMODE FILTER'S PROFILE — 20 Hz to 20 kHz — so a player
/// comparing this card's curve with the filter card's is comparing them on one axis.
namespace flt = feathers::fx::filter;

namespace feathers::ui {

namespace {

/// **THE TWO CONTROLS THAT ARE ENUMERATIONS RATHER THAN AMOUNTS.** Declared once, here, so the
/// constructor and the layout cannot disagree about which cells hold a box.
constexpr bool isChoiceCell(amd::Param p) {
	return p == amd::Param::SyncDivision || p == amd::Param::LfoMode;
}

/// A CAPTION IN THE ROW'S VOICE. `knob.cpp` keeps its own file-local `upper` for the same job; this
/// is that function rather than a shared one, because promoting a four-line helper to a header to
/// avoid four lines is a worse trade than the duplication.
std::string upperCaption(const char* text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return out;
}

/// HOW FAR GAMMA'S BOX IS PULLED IN FROM ITS CELL. The only number here that is a taste decision;
/// its VERTICAL placement is not — see `layOut`, which takes `Knob`'s own arithmetic.

/// HOW FAR GAMMA'S PLATE STANDS OUTSIDE ITS BOX. Outside rather than around, because the box paints
/// its own ground over its own bounds — see `draw`.
constexpr float kGammaPlatePad = 4.0f;

/// HOW MANY LAPS OF THE FIELD A FULL DRAG OF GAMMA COVERS. See the constructor for why sixteen, and
/// for what it means about the top of the range.

/// THE BOTTOM ROWS: what plays the coordinates. Nothing here changes what the device IS.
///
/// SYNC SITS BESIDE RATE, because it decides what the Rate knob MEANS — off and Rate is a frequency,
/// on and Rate is ignored. Two controls answering one question should be adjacent or a hand has to
/// hunt for the other half.
///
/// AND THE ORDER IS THE WRAP, chosen so ONE order serves BOTH grids: the first three are WHEN the
/// LFO runs, the rest are HOW MUCH is heard, and the split falls in the same place whether the rows
/// are 3/3/1 or 3/4. **`LFO MODE` is deliberately in the first three**, because it is the only
/// caption that does not fit a four-wide cell — 53.32px measured against 54 — so it must land in a
/// row of three wherever the grid puts it.
constexpr std::array<amd::Param, AutomodCard::kBottomCells> kCardKnobs {
	amd::Param::Rate, amd::Param::SyncDivision, amd::Param::LfoMode,
	amd::Param::Freq, amd::Param::Depth,        amd::Param::Manual, amd::Param::Mix,
};
static_assert(kCardKnobs.size() == AutomodCard::kBottomCells,
              "the grid reserves room for exactly the controls this array builds");

} // namespace

AutomodCard::AutomodCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                         Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	// THE REGISTRY IS TAKEN AND NOT KEPT, which is what `Knob`'s device-control constructor already
	// did and said: *"a device's control is not a host parameter, so there is no clap_id for the
	// registry to key on; whoever built the device repaints its widgets through the same ModState this
	// knob is later handed."* This card's knobs are all on that road, so a stored reference was only
	// ever passed along to be ignored — and once `Knob` stopped taking it, storing it here became
	// provably dead. Kept in the SIGNATURE so every call site reads alike and a card can move to the
	// parameter road without its surroundings changing.
	//
	// NOT A MISSING CALL, which is the other thing an init-only member usually means and is the
	// dangerous reading: the registry is the parameter->widget REPAINT road, so a card that had meant
	// to subscribe and forgotten would go stale under automation. Excluded by reading the
	// constructor that took it — it never called `watch()` and said so.
	(void)registry;
	controls_.resize(static_cast<size_t>(amd::Param::kCount));

	// THE PICTURE, BUILT FIRST. It is not in `cells_`: that list is every widget a HAND can reach,
	// and the strip is ink. Its visibility is therefore swept by `setFolded` beside the cells rather
	// than by the loop over them.
	strip_ = std::make_unique<AutomodStrip>(host_, device_);
	addChild(strip_.get());


	// THROUGH THE SHARED SEATING — see `automod_coords.h`. BY DESIGN the window shows the same
	// coarse/fine pairs and displays the same values as the card, which is a promise only one
	// construction can keep.
	for (const auto& spec : automod_coords::specs()) {
		auto coord = automod_coords::makeCoordCell(
		    spec, [this](amd::Param p) -> DeviceControl& { return control(p); }, kKnobDiameter);
		addChild(coord.get());
		cells_.push_back(coord.get());
		coords_.push_back(std::move(coord));
		// ONE PARAM PER CELL NOW — BJ piece 3.
		coordParams_.push_back(spec.coordinate);
	}

	// GAMMA: ONE BOX, NO COLON, AND **NO CAPTION OF ITS OWN** — the ROW names it, so the label stays
	// empty, which is `GammaBox`'s default. Everything else that used to be spelled here — compact,
	// and the sixteen-laps drag span with the reasoning behind it — MOVED to `gamma_box.h` rather than
	// being copied, because the window that was built later never looked for it and drew a knob.
	gamma_ = std::make_unique<GammaBox>(control(amd::Param::Gamma));
	addChild(gamma_.get());
	cells_.push_back(gamma_.get());

	for (const amd::Param p : kCardKnobs) {
		const char* label = amd::kParamDescs[static_cast<size_t>(p)].name;
		if (isChoiceCell(p)) {
			// **A BOX, NOT A KNOB, BECAUSE THESE ARE ENUMERATIONS.** They are dropdowns; the house
			// already answered this exact requirement on the MSEG card, whose header records it:
			// *"A BOX RATHER THAN A CHIP, because the chips are switches and this is a choice among
			// nine. The box already knows how: `formatValue` renders `Unit::Choice` by name, so it
			// reads '1 bar' rather than '6'."* There is no separate dropdown widget in this editor and
			// building one would be a second widget for a job `ValueBox` already does.
			//
			// EMPTY LABEL AND COMPACT, exactly as gamma is: the ROW names it, on the shared label line
			// the knobs beside it use, so a box and a knob in one row read as one row.
			auto box = std::make_unique<ValueBox>(control(p), "");
			box->setCompact(true);
			addChild(box.get());
			cells_.push_back(box.get());
			choiceParams_.push_back(p);
			choices_.push_back(std::move(box));
			continue;
		}
		auto knob = std::make_unique<Knob>(control(p), label, notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for.
		knobParams_.push_back(p);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A PRESS THAT IS NOT ON A VERB MAY BECOME A CARD DRAG — the lane's reorder gesture. Noted
		// first, and each verb below returns, so a press that does something does not also pick the
		// card up.
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		// THE TOGGLE, THE GESTURE AND THE REDRAW LIVE IN THE BASE — see FxCard::enablePressAt.
		// This card keeps only its own voice, which the base holds no notifier to speak with.
		if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Automod on" : "Automod off");
			}
			return;
		}
		if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			return;
		}
	};
}

device::Modulatable* AutomodCard::enableModulatable() {
	return hasEnable() ? &control(static_cast<amd::Param>(enableControl())) : nullptr;
}

DeviceControl& AutomodCard::control(amd::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, amd::kDescriptor, i);
	}
	return *controls_[i];
}

visage::Bounds AutomodCard::headBounds() const { return ownFrame().head; }

float AutomodCard::openHeight() const {
	const float column = Knob::columnHeight(kKnobDiameter);
	// TWO ROWS OF THE SAME COLUMN HEIGHT — a coordinate box borrows a knob's row structure exactly
	// (`zone_coord_box.h` records that, so the two stack), so one number serves both.
	// THE STRIP AND ITS GAP ARE COUNTED HERE AND NOWHERE ELSE; `layOut` takes its first row's y from
	// `lfoStripBounds().bottom()`, so the two cannot disagree about how much room the picture took.
	// THE PICTURE, THE PERFORMANCE ROWS, AND THE COORDINATE ROW — counted from `performanceRows`
	// rather than spelled out, so a grid change moves the height with it.
	//
	// THE ROWS THIS CARD ACTUALLY HAS. Asked of the layout in force rather than reserved for the
	// worst case — see the header for what the static form cost.
	const size_t rows = performanceRows(layout_).size();
	return kSlotHeadHeight + kStripHeight + kRowGap + column
	       + static_cast<float>(rows) * (kRowGap + column) + kPad;
}

std::span<const size_t> AutomodCard::performanceRows(Layout which) {
	// SEVEN CONTROLS. **`Env` WENT TO THE WINDOW**, because the trim cluttered the card — and the
	// row counts go back to what they were before it
	// arrived, which is the whole point of moving it.
	//
	// THE HISTORY IS WORTH ONE PARAGRAPH, because the eighth cell cost more than it looked like it
	// would and the next person to add one should know. `{3, 4, 1}` looked like the natural home and
	// broke the card: three performance rows plus the coordinate row is four, and
	// `test_gui_widgets.cpp` pins the open height BELOW strip + four knob columns — 396px measured
	// against a 330px bar. So it went in the second row instead, at `{3, 5}`, and a five-wide cell is
	// ~43px, which is why it was captioned `Env` and not `Env Trim`. **A card that has to shorten a
	// caption to fit a control is a card telling you the control belongs somewhere else.**
	//
	// `LFO MODE` still fixes the first row at three — 53.32px measured against the 54px a four-wide
	// cell gives — so that constraint outlives this change and is not a leftover.
	static constexpr size_t kUniform[] = { 3, 3, 1 };
	static constexpr size_t kCompact[] = { 3, 4 };
	return which == Layout::Uniform ? std::span<const size_t> { kUniform }
	                                : std::span<const size_t> { kCompact };
}

bool AutomodCard::isChoiceBox(const visage::Frame* frame) const {
	for (const std::unique_ptr<ValueBox>& box : choices_) {
		if (box.get() == frame) {
			return true;
		}
	}
	return false;
}

void AutomodCard::setLayout(Layout which) {
	if (layout_ == which) {
		return;
	}
	layout_ = which;
	layOut();
	redraw();
}

void AutomodCard::foldedChanged() {
	// EVERY CELL, NOT EVERY KNOB. A folded card that left its coordinate boxes drawn would leave
	// controls a hand can still reach over whatever the lane put underneath. `cells_` is every widget
	// this card built regardless of kind — a list that has to be remembered separately is a list that
	// will be forgotten the next time a widget kind arrives.
	for (visage::Frame* cell : cells_) {
		cell->setVisible(!folded_);
	}
	// AND THE PICTURE, which is not a cell — see the constructor. A folded card that left the strip
	// drawn would paint a waveform over whatever the lane put underneath it.
	strip_->setVisible(!folded_);
	layOut();
	// AND THE LANE IS TOLD, because a fold changes this card's HEIGHT and the lane places cards by
	// asking for it. THIS LINE WAS MISSING and `EVERY card in the lane folds to its head` caught it:
	// the card reported 186px where 24 was required, because `redraw()` alone repaints at the size it
	// already had. The symptom, as seen on another card: the UI goes away, but the card size does not
	// change.
}

void AutomodCard::resized() { layOut(); }

bool AutomodCard::updateContent() {
	// THE STRIP, NOT THIS CARD. Nothing on the card's own surface moves with the LFO, so redrawing
	// the card would repaint a head, three coordinate boxes and seven knobs to move one star inside a
	// child. `OwlttCard::update` says the same sentence about its graph.
	//
	// **AND NOT AT ALL WHILE FOLDED, WHICH IS A CLAIM ABOUT WHAT THE COUNT MEANS.** `redraw()` is
	// refused for a frame that is not visible, so a `true` returned here for a folded card would be
	// a frame TOLD and not a frame DRAWN — playbook 8v, which is the distinction that made "79% of
	// the work is collateral" the wrong sentence. `FxLane::updateCards()` counts what this returns.
	return !folded_ && strip_ != nullptr && strip_->update();
}

void AutomodCard::layOut() {
	// THE PICTURE IS SIZED FROM THE CARD'S OWN GEOMETRY, and BEFORE the folded early-out: folded,
	// `lfoStripBounds()` is empty and the strip collapses with it, which is what stops a hidden frame
	// keeping the bounds it had when the card was open.
	const visage::Bounds strip = lfoStripBounds();
	strip_->setBounds(strip.x(), strip.y(), strip.width(), strip.height());
	if (folded_) {
		return;
	}
	const float column = Knob::columnHeight(kKnobDiameter);
	const float usable = std::max(bodyWidth() - 2.0f * kPad, 0.0f);

	// **THE PERFORMANCE ROWS COME FIRST AND THE COORDINATES LAST** — Type, Flavour and Mod on the
	// bottom row. Reading down: the picture, the things that PLAY it, then the things that AUTHOR
	// it.
	//
	// EACH PERFORMANCE ROW SIZES ITSELF FROM ITS OWN CELL COUNT, which is what lets a 3 sit above a 4:
	// the cell width is per row rather than per card, so `LFO MODE` gets a 72px cell in the row of
	// three while `MIX` takes 54 in the row of four.
	const float topCell = usable / static_cast<float>(kTopCells);
	const std::span<const size_t> rows = performanceRows(layout_);

	float y = lfoStripBounds().bottom() + kRowGap;
	size_t placed = 0;
	choiceCells_.clear();
	for (const size_t inRow : rows) {
		const float cell = usable / static_cast<float>(std::max<size_t>(inRow, 1));
		for (size_t c = 0; c < inRow && placed < kBottomCells; ++c, ++placed) {
			// THE KNOBS SIT AFTER THE COORDINATE CELLS IN `cells_`, which is the order they were BUILT
			// in; where they are DRAWN is this loop's business and nothing else's.
			visage::Frame* cell_ = cells_[kTopCells + placed];
			const float cx = bodyLeft() + kPad + cell * static_cast<float>(c);
			// A CHOICE BOX IS CENTRED IN THE SPACE A KNOB'S FACE WOULD OCCUPY — gamma's arithmetic,
			// which is what puts its text on its neighbours' readout line and leaves the shared label
			// line free for the row to write on.
			if (isChoiceBox(cell_)) {
				choiceCells_.push_back({ cx, y, cell, column });
				cell_->setBounds(GammaBox::cellBounds({ cx, y, cell, column }, kKnobDiameter));
			}
			else {
				cell_->setBounds(cx, y, cell, column);
			}
		}
		y += column + kRowGap;
	}

	// AND THE COORDINATE ROW BENEATH THEM.
	for (size_t i = 0; i < kTopCells && i < cells_.size(); ++i) {
		// **FROM THE BODY'S LEFT EDGE, NOT THE CARD'S.** On a spine the head takes the first 24px and
		// a row starting at `kPad` starts underneath it — which is the reported defect, and it survived one
		// round of this fix because the sibling loop above was patched and this one was not.
		const float x = bodyLeft() + kPad + topCell * static_cast<float>(i);
		if (cells_[i] == gamma_.get()) {
			// GAMMA'S BOX IS COMPACT AND CENTRED IN THE SPACE A KNOB'S FACE WOULD OCCUPY, which is
			// what puts its number on the same line as its neighbours' and leaves the shared label
			// line free for the row to write `GAMMA` on — at the arithmetic its neighbours use
			// (`kKnobDiameter + kGap`), which a photograph once caught being the tray's instead.
			gammaCell_ = { x, y, topCell, column };
			cells_[i]->setBounds(GammaBox::cellBounds(gammaCell_, kKnobDiameter));
		}
		else {
			cells_[i]->setBounds(x, y, topCell, column);
		}
	}
}

void AutomodCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// EVERY ADAPTER THE CARD OWNS, swept unconditionally — FilterCard's total-sweep rule, and it is
	// what covers the coordinate boxes' COARSE halves, which no knob has ever held.
	for (size_t i = 0; i < static_cast<size_t>(amd::Param::kCount); ++i) {
		control(static_cast<amd::Param>(i)).setModulation(mod, history);
	}
	for (const std::unique_ptr<Knob>& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	redraw();
}

visage::Bounds AutomodCard::lfoStripBounds() const {
	if (folded_) {
		return {};
	}
	return {bodyLeft() + kPad, kSlotHeadHeight, std::max(bodyWidth() - 2.0f * kPad, 0.0f), kStripHeight };
}

visage::Bounds AutomodCard::responseStripBounds() const {
	// THE SAME BOX. "Same field" is the design, so there is no second rectangle to keep in step with
	// the first — the two pictures share one, and what separates them is their axes rather than their
	// geometry. WHICH picture is the strip's business; WHERE it sits is the card's, and a folded card
	// has neither.
	return (folded_ || strip_->responsePlot().width() <= 0.0f) ? visage::Bounds {}
	                                                           : lfoStripBounds();
}

bool AutomodCard::headEnabled() const {
	return !hasEnable() || control(static_cast<fx::automod::Param>(enableControl())).value() >= 0.5;
}

void AutomodCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void AutomodCard::drawContent(visage::Canvas& canvas) {
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.


	// THE PICTURE IS A CHILD AND DRAWS ITSELF. It is hidden while folded — see `setFolded` — which
	// is what the `if (!folded_)` around the three draw calls used to say.

	// GAMMA'S OWN PLATE, DRAWN BY THE CARD — because the compact `ValueBox` deliberately draws none.
	//
	// `value_box.cpp` says why, and its premise is honest about itself: *"A compact box is seated
	// INSIDE something that already framed it, so it reads as a recess in that surface rather than as
	// a card laid on top of it — same fill, no border below."* That holds where compact mode was
	// born. **HERE NOTHING FRAMES IT**: gamma sits directly on the card surface beside three
	// `ZoneCoordCell`s that each carry their own plate, and same-fill-on-the-same-surface is not a
	// recess, it is nothing. The photograph showed `0.00` as bare text — neither a knob nor a box,
	// and therefore not visibly typeable, which is the whole affordance the design rests on.
	//
	// SO THE CARD SUPPLIES THE EDGE, at the same colour, radius and weight its neighbours use, and
	// **drawn OUTSIDE the box's own rectangle** — the box fills `kSurface` over its own bounds, so a
	// border on those bounds would be painted over the moment the child draws.
	//
	// NOT A `value_box` CHANGE, deliberately: compact mode is correct for its own callers and this is
	// the CARD knowing something the widget cannot — whether anything frames it. A shared file edited
	// for one caller's context is how a widget acquires a flag for every place it is used.
	if (gamma_ != nullptr) {
		const visage::Bounds b = gamma_->bounds();
		const float pad = kGammaPlatePad;
		canvas.setColor(withAlpha(kLine, 0.12f));
		canvas.roundedRectangle(b.x() - pad, b.y() - pad, b.width() + 2.0f * pad,
		                        b.height() + 2.0f * pad, ZoneCoordCell::kPlateRadius);
		canvas.setColor(withAlpha(kLine, 0.7f));
		canvas.roundedRectangleBorder(b.x() - pad, b.y() - pad, b.width() + 2.0f * pad,
		                              b.height() + 2.0f * pad, ZoneCoordCell::kPlateRadius, 1.0f);
	}

	// THE ROW NAMES GAMMA, on the same label line its three neighbours use. The box carries only the
	// number — see the constructor for why, and `layOut` for why the y is this row's face diameter
	// rather than a measurement.
	if (gamma_ != nullptr && gammaCell_.width() > 0.0f) {
		const visage::Font& face = font(kKnobLabelSize);
		canvas.setColor(kMuted);
		const std::string caption = upperCaption("Gamma");
		const float w = trackedWidth(face, caption, kKnobLabelTracking);
		drawTracked(canvas, caption, face, kKnobLabelTracking,
		            gammaCell_.x() + (gammaCell_.width() - w) * 0.5f,
		            gammaCell_.y() + kKnobDiameter + Knob::kGap, Knob::kLabelHeight);
	}

	// EACH CHOICE BOX IS NAMED BY THE ROW, on the same label line its knob neighbours use. The box
	// carries only the value — `formatValue` renders `Unit::Choice` BY NAME, so it reads "1/8" and
	// "Retrigger" rather than "11" and "1".
	//
	// AND THE CARD SUPPLIES THE PLATE, for the reason gamma's does: a compact box draws no border,
	// because compact mode was designed for a box already seated inside something that framed it, and
	// nothing frames these. Same colour, radius and weight as gamma's, drawn OUTSIDE the box's own
	// bounds because the box fills its own rectangle.
	{
		const visage::Font& face = font(kKnobLabelSize);
		for (size_t i = 0; i < choices_.size() && i < choiceCells_.size(); ++i) {
			const visage::Bounds b = choices_[i]->bounds();
			const float pad = kGammaPlatePad;
			canvas.setColor(withAlpha(kLine, 0.12f));
			canvas.roundedRectangle(b.x() - pad, b.y() - pad, b.width() + 2.0f * pad,
			                        b.height() + 2.0f * pad, ZoneCoordCell::kPlateRadius);
			canvas.setColor(withAlpha(kLine, 0.7f));
			canvas.roundedRectangleBorder(b.x() - pad, b.y() - pad, b.width() + 2.0f * pad,
			                              b.height() + 2.0f * pad, ZoneCoordCell::kPlateRadius, 1.0f);

			const std::string caption = upperCaption(
			    amd::kParamDescs[static_cast<size_t>(choiceParams_[i])].name);
			const float w = trackedWidth(face, caption, kKnobLabelTracking);
			canvas.setColor(kMuted);
			drawTracked(canvas, caption, face, kKnobLabelTracking,
			            choiceCells_[i].x() + (choiceCells_[i].width() - w) * 0.5f,
			            choiceCells_[i].y() + kKnobDiameter + Knob::kGap, Knob::kLabelHeight);
		}
	}

}

} // namespace feathers::ui
