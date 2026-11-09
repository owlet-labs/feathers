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


#include "plugin/ui/automod_expanded.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/automod_coords.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace amd = fx::automod;

namespace {

/// THE KNOB ROW, IN ORDER: the three coordinates first, then the two controls that act on all three.
///
/// THE ORDER IS THE CLAIM THE PANEL MAKES. Type, Flavour and Mod are WHERE you are; Gamma and Rotate
/// are what is done to all three at once — gamma slides every phase together, rotate re-aims the
/// basis they are projected onto. Reading left to right is reading from the coordinate outward.
///
/// **THE ROWS BELOW THE PAD.** Row 1 is the coordinate row — built separately, because its cells are
/// `ZoneCoordBox`es and a `GammaBox`, not knobs. These are rows 2, 3 and 4.
///
/// ROWS 2 AND 3 ARE THE CARD'S PERFORMANCE GRID, IN THE CARD'S OWN ORDER AND ITS OWN `{3, 4}` SPLIT.
/// BY DESIGN: the window is a superset of the card, and a superset that rearranges is a different kind of
/// unhelpful — a hand that knows the card should find these where it expects them. `Sync` sits beside
/// `Rate` because it decides what Rate MEANS, which is the card's reason and still holds here.
///
/// ROW 4 IS WHAT ONLY THE WINDOW HAS: `Env`, which trims what the follower hears and was moved off the
/// card deliberately. `Rotate` was here too and now sits INSIDE the pad (backlog IJ, the shaper's
/// placement) — it re-aims the plane the pad shows, so it belongs on the picture it turns.
/// `Width` sits beside Env (backlog IS): how far apart the two ears' modulation runs, which the card has no
/// room to explain and the window does.
constexpr std::array<amd::Param, 3> kRowRate {
	amd::Param::Rate, amd::Param::SyncDivision, amd::Param::LfoMode,
};
constexpr std::array<amd::Param, 4> kRowDepth {
	amd::Param::Freq, amd::Param::Depth, amd::Param::Manual, amd::Param::Mix,
};
/// ONE UNIT OF ROTATE PER FULL DRAG — the shaper's `kShaperFieldDragSpan`, named here for the same
/// reason it is named there: sixteen (GammaBox's default) is a different gesture entirely.
constexpr double kRotateDragSpan = 1.0;

constexpr std::array<amd::Param, 3> kRowWindowOnly {
	amd::Param::EnvTrim, amd::Param::Width, amd::Param::Keytrack,
};
static_assert(AutomodExpanded::kCoordCells + kRowRate.size() + kRowDepth.size()
                  + kRowWindowOnly.size()
              == AutomodExpanded::kCellCount,
              "the grid reserves room for exactly the cells these arrays build");

/// THE THREE THE PAD MOVES. Named here rather than at each use, because the reachability list, the
/// pad's construction and this file's comments must all mean the same three.
///
/// **THE COORDINATES THEMSELVES NOW, WHERE THIS NAMED THEIR PHASE HALVES — backlog BJ, piece 3.** The
/// phases were absorbed into the coordinates, so the three the pad's plane is computed from and the
/// three a surface must reach are one list rather than two that happened to correspond.
constexpr std::array<amd::Param, 3> kPadParams {
	amd::Param::Type, amd::Param::Flavor, amd::Param::Mod,
};

/// The device qualifies nothing on this device, so a short label is the name — but the road is the
/// same one the shaper takes, so a rename there does not have to be discovered here.
const char* shortLabelOf(amd::Param p) { return amd::kParamDescs[static_cast<size_t>(p)].name; }

} // namespace

AutomodExpanded::AutomodExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                 Notifier* notifier)
    : host_(host), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — the same note ShaperExpanded carries.
	(void)registry;
	controls_.resize(static_cast<size_t>(amd::Param::kCount));

	// ROW 1 — THE COORDINATE ROW, THROUGH THE SHARED SEATING. BY DESIGN they are the same
	// coarse/fine pairs and display the same values as on the card. Not a look-alike: the same
	// construction, in `automod_coords.h`, which the card also calls. Two surfaces cannot drift when
	// there is one seating.
	//
	// **AND THIS IS WHAT REPLACED THREE BARE KNOBS.** The window showed Type, Flavour and Mod as plain
	// dials with their phases reachable only by aiming the pad — they read as local phase knobs
	// with no phase offset. The structural superset test scored those three phases as missing
	// from the window, which they were.
	for (const auto& spec : automod_coords::specs()) {
		auto coord = automod_coords::makeCoordCell(
		    spec, [this](amd::Param p) -> DeviceControl& { return control(p); }, kKnobDiameter);
		addChild(coord.get());
		cells_.push_back(coord.get());
		// ONE PARAM PER CELL NOW — BJ piece 3.
		coordParams_.push_back(spec.coordinate);
		coords_.push_back(std::move(coord));
	}

	// GAMMA CLOSES THE COORDINATE ROW, as it does on the card — a box, never a knob, see `gamma_box.h`.
	// A LABEL IS PASSED HERE and is not on the card: this grid captions each cell under the widget,
	// where the card writes `GAMMA` on a shared label line and an inner caption would name it twice.
	gamma_ = std::make_unique<GammaBox>(control(amd::Param::Gamma), shortLabelOf(amd::Param::Gamma));
	addChild(gamma_.get());
	cells_.push_back(gamma_.get());

	// ROWS 2, 3 AND 4 — knobs, in row order.
	const auto addKnob = [&](amd::Param p) {
		auto knob = std::make_unique<Knob>(control(p), shortLabelOf(p), notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		knobParams_.push_back(p);
	};
	for (const amd::Param p : kRowRate) {
		addKnob(p);
	}
	for (const amd::Param p : kRowDepth) {
		addKnob(p);
	}
	for (const amd::Param p : kRowWindowOnly) {
		// **KEYTRACK IS A SWITCH, SO A CHIP** — a two-state control, which the multivalue-toggle rule allows
		// (backlog IS.5). Everything else on this row is a knob.
		if (p == amd::Param::Keytrack) {
			keytrack_ = std::make_unique<SwitchChip>(control(p), shortLabelOf(p));
			addChild(keytrack_.get());
			cells_.push_back(keytrack_.get());
			continue;
		}
		addKnob(p);
	}

	// THE PLANE: Type across, Flavour up — by design. The pad writes those two coordinates directly now;
	// the polar decode and its three phase offsets are gone, and so is the rotate the circle needed.
	// **THE PAD READS AND WRITES THE ROW, NOT TWO CONTROLS.** Its displacement is a 39-vector; there
	// is no pair of knobs it could be expressed through. The host's `setField` dispatches a row index
	// past the declared params onto the blob road — see `BridgeDeviceHost::setField`.
	pad_ = std::make_unique<AutomodPad>(
	    [this](size_t index, float value) {
		    host_.setField(device_, static_cast<size_t>(amd::Param::kCount) + index, value);
	    },
	    [this](size_t field) { return host_.fieldAt(device_, field); }, control(amd::Param::Rotate));
	addChild(pad_.get());
	// ONE DRAG OF THE PAD IS ONE UNDO STEP (backlog JB), as the shaper's.
	pad_->onHoldBegan = [this] {
		padGesture_.begin(host_, device_, static_cast<size_t>(amd::Param::kCount), AutomodPad::kTravelCount);
	};
	pad_->onHoldEnded = [this] { (void)padGesture_.end(host_, history_, "Pad"); };
	// **ROTATE, INSIDE THE TERRAIN BOX** (backlog IJ, the shaper's placement: Rotate sits close to or even
	// inside the terrain box, and the automod pad gets the same treatment) — a
	// compact box in the pad's top-right corner, made AFTER the pad so it draws over the map it turns.
	//
	// **A BOX, FOR THE REASON GAMMA IS ONE: the quantity is unbounded.** Its descriptor declares a
	// symmetric drag window rather than a domain, and `Knob::dragTo` maps one drag across the WHOLE
	// declared range. **ONE UNIT PER DRAG** — `GammaBox` defaults to sixteen laps, and the shaper
	// measured at 5-degree resolution that one unit of rotate covers 24 distinct planes and sixteen 412.
	rotate_ = std::make_unique<GammaBox>(control(amd::Param::Rotate), shortLabelOf(amd::Param::Rotate));
	rotate_->setDragSpan(kRotateDragSpan);
	addChild(rotate_.get());
	// THE ENABLE SQUARE THAT HAD A PRESS HERE IS THE WINDOW HEADER'S CHIP NOW (2026-09-26), one for
	// every device — and it announces in the device's name, as this did.
}

DeviceControl& AutomodExpanded::control(amd::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] =
		    std::make_unique<DeviceControl>(host_, device_, amd::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

void AutomodExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the enable is a gesture and the pad's
	// three phases are written by a drag, so an adapter that never learned the panel's state would
	// report itself un-armable forever. ShaperExpanded's rule, and its reason.
	for (size_t i = 0; i < static_cast<size_t>(amd::Param::kCount); ++i) {
		control(static_cast<amd::Param>(i)).setModulation(mod, history);
	}
}

visage::Bounds AutomodExpanded::padBounds() const {
	return {kPad, kPad + kSwitchHeight + kRowGap, kPadSide, kPadSide};
}

visage::Point AutomodExpanded::maximumSize() const {
	const visage::Point natural = minimumSize();
	return { natural.x + kPad, natural.y + kPad };
}

visage::Point AutomodExpanded::minimumSize() const {
	// STATED AS THE ROWS THIS PANEL LAYS OUT rather than measured after the fact, because the overlay
	// CLAMPS rather than scrolling — a panel asking for more than it fills leaves blank canvas, and
	// one asking for less has its bottom row silently cut off.
	const std::span<const size_t> rows = cellRows();
	const float column = Knob::columnHeight(kKnobDiameter);
	// EVERY ROW COUNTED, not one — the panel grew from a single knob row to four when it became a
	// superset of its card, and a height that still described one row would have had the overlay clamp
	// three of them off the bottom. The overlay CLAMPS rather than scrolling, so a row that does not
	// fit is not merely ugly, it is unreachable.
	float height = kPad + kSwitchHeight + kRowGap + kPadSide;
	for (size_t i = 0; i < rows.size(); ++i) {
		height += kRowGap + column;
	}
	height += kPad;

	const float padRow = kPad + kPadSide + kPad;
	size_t widest = 0;
	for (const size_t n : rows) {
		widest = std::max(widest, n);
	}
	const float knobRow = kPad + static_cast<float>(widest) * kCellWidth + kPad;
	// WHICHEVER ROW IS WIDER WINS, so a row growing cannot silently overflow the frame.
	return {std::max(padRow, knobRow), height};
}

std::span<const size_t> AutomodExpanded::cellRows() {
	// FOUR ROWS. The coordinate row, the card's `{3, 4}` performance split, then the window's own two.
	static constexpr size_t kRows[] = { kCoordCells, 3, 4, 3 };
	return kRows;
}

std::string AutomodExpanded::title() const {
	// THE DEVICE'S OWN NAME, asked of the descriptor rather than spelled here, so the panel and the
	// card's head cannot come to say different things.
	return amd::kTraits.name;
}

void AutomodExpanded::resized() { layOut(); }

void AutomodExpanded::layOut() {
	pad_->setBounds(padBounds());
	// ROTATE IN THE TERRAIN BOX'S TOP-RIGHT CORNER, inset so the map's border stays visible around it —
	// the shaper's placement and the shaper's numbers.
	{
		const visage::Bounds pad = padBounds();
		constexpr float kInset = 6.0f;
		constexpr float kRotateWidth = 118.0f;
		rotate_->setBounds(pad.right() - kRotateWidth - kInset, pad.y() + kInset, kRotateWidth,
		                   rotate_->naturalHeight());
	}
	const float column = Knob::columnHeight(kKnobDiameter);
	const float usable = std::max(width() - 2.0f * kPad, 1.0f);
	const std::span<const size_t> rows = cellRows();
	size_t widest = 1;
	for (const size_t n : rows) {
		widest = std::max(widest, n);
	}
	// EVERY ROW ON THE SAME CELL WIDTH, taken from the WIDEST row, so the columns line up down the
	// panel instead of each row spreading to fill. A four-cell row and a two-cell row that both
	// stretched would put no two captions above one another.
	const float cell = std::max(kCellWidth, usable / static_cast<float>(widest));

	float y = padBounds().bottom() + kRowGap;
	size_t i = 0;
	for (const size_t inRow : rows) {
		for (size_t c = 0; c < inRow && i < cells_.size(); ++c, ++i) {
			const visage::Bounds box { kPad + cell * static_cast<float>(c), y, cell, column };
			// GAMMA'S BOX IS CENTRED IN THE SPACE A KNOB'S FACE WOULD FILL — one arithmetic, shared
			// with the card, in `GammaBox::cellBounds`.
			// KEYTRACK'S CHIP SITS ON THE KNOBS' FACE LINE, centred where a face would be — a chip is shorter.
			if (cells_[i] == keytrack_.get()) {
				const float faceMid = box.y() + kKnobDiameter * 0.5f;
				keytrack_->setBounds(box.x(), faceMid - SwitchChip::kHeight * 0.5f, box.width(), SwitchChip::kHeight);
				continue;
			}
			cells_[i]->setBounds(cells_[i] == gamma_.get() ? GammaBox::cellBounds(box, kKnobDiameter)
			                                              : box);
		}
		y += column + kRowGap;
	}
}

std::vector<amd::Param> AutomodExpanded::addressedParamsForTest() const {
	std::vector<amd::Param> out = knobParams_;
	// THE PAD'S THREE, which no knob holds — a drag writes them and a widget walk cannot see that.
	out.insert(out.end(), kPadParams.begin(), kPadParams.end());
	// AND ROTATE, WHICH STOPPED BEING A KNOB when it stopped being bounded. It is reachable through
	// its own box; a list built from `knobParams_` alone would report it unreachable and the
	// reachability sweep would be right to say so.
	out.push_back(amd::Param::Rotate);
	// AND KEYTRACK, A CHIP RATHER THAN A KNOB (backlog IS.5) — reachable through it, invisible to `knobParams_`.
	out.push_back(amd::Param::Keytrack);
	// (AND NOT THE ENABLE: the window header's chip reaches it, for every panel.)
	return out;
}

void AutomodExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.rectangle(0.0f, 0.0f, width(), height());

	// THE PAD'S CAPTION. The enable square that sat before it is the window header's chip now.
	canvas.setColor(kInk);
	canvas.text("PHASE PLANE", font(kKnobLabelSize), visage::Font::kLeft, kPad, kPad, 220.0f, kSwitchHeight);
}

} // namespace feathers::ui
