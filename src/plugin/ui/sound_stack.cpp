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

#include "plugin/ui/param_edit.h"
#include "plugin/ui/slot_edit.h"
#include "plugin/ui/stored_edit.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/sound_stack.h"

#include "plugin/sound_hues.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/celestial.h"
#include "plugin/params.h"

#include <visage/graphics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {

namespace {

/// The mock's `.lbl`, whose tracking is `.26em` — a hair tighter than the byline's `.28em`. Kept
/// apart because they are different roles: this labels a rail, the byline signs the instrument.
constexpr float kRailLabelTracking = 0.26f;

/// `.seed { border-radius: 8px }` and the 6px of `.anc-row`.
constexpr float kCardRounding = 8.0f;
constexpr float kBorder = 1.0f;

/// `.seed.sel { border-color: #33406a }` and `.sw.on { border-color: #3A4C74 }`: the mock's literals, named once in
/// `theme.h` (`kBorderSelected`, `kBorderOn`) because no generated token carries them.
constexpr uint32_t kSelectedBorder = kBorderSelected;
constexpr uint32_t kSwitchOnBorder = kBorderOn;

/// `.seed .dot { width: 9px }`, and how far its glow reaches (`box-shadow: 0 0 7px`).
constexpr float kDotSize = 9.0f;
constexpr float kDotGlow = 7.0f;

/// `.seed.off .zn, .seed.off .dot { opacity: .35 }`.
constexpr float kDisabledOpacity = 0.35f;

/// The two type sizes a card uses: the mock's `.seed .zn` (11px) and `.anc-row` (9px).
constexpr float kCardReadoutSize = 11.0f;
constexpr float kCardSubSize = 9.0f;

} // namespace

SoundStack::SoundStack(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection,
                       EditHistory& history, FrameWatchers& previewWatchers, Notifier* notifier)
    : bridge_(bridge), selection_(selection), history_(history), previewWatchers_(previewWatchers),
      notifier_(notifier) {
	// The rail draws the preview state too — the ring, and the fallback on everything else — so it is
	// one of its own watchers rather than repainting itself by hand at each call site.
	watches_.push_back(previewWatchers_.watch(this));
	// Every field a card prints, for every slot: the readouts follow automation and preset loads as
	// well as their own switch, which is the same rule the Engine rail's enablement follows.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (const SoundField field : { SoundField::Zone, SoundField::Offset, SoundField::Gamma,
			                            SoundField::PlaceX, SoundField::PlaceY, SoundField::Enable,
			                            SoundField::Width }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(soundParam(slot, field)), this));
		}
	}
	watches_.push_back(selection_.watch(this));
	// A KIT'S CARDS: each its pad's Level fader (shown in Kit mode only, laid out with the cards), and the mode, every pad's
	// Mute, Kind and Note watched, because a kit card draws them.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const ParamDesc* level = findParam(static_cast<clap_id>(drumParam(slot, DrumControl::Level)));
		levelControls_[slot] = std::make_unique<ParamControl>(bridge, level);
		levelFaders_[slot] = std::make_unique<FaderPill>(*levelControls_[slot], "Level");
		// A REAL RECTANGLE FROM THE START (a zero-size child is what the create()-positions guard hunts), hidden until a kit.
		levelFaders_[slot]->setBounds(0.0f, 0.0f, kWidth * 0.5f, kCardSubSize + 4.0f);
		levelFaders_[slot]->setVisible(false);
		addChild(levelFaders_[slot].get());
		watches_.push_back(registry.watchToken(static_cast<clap_id>(level->id), levelFaders_[slot].get()));
		for (const DrumControl c : { DrumControl::Mute, DrumControl::Kind, DrumControl::Note }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, c)), this));
		}
	}
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::OscType), this));

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// `position`, not `relativePosition()` — the latter is how far the mouse MOVED, which is zero
		// on a press. See the note in param_row.cpp; it cost a release once.
		const Hit hit = hitAt(e.position);
		switch (hit.part) {
		case Part::Enable:
			if (kitMode()) {
				togglePadMute(hit.slot);
			}
			else {
				toggleEnable(hit.slot);
			}
			break;
		case Part::Clone:
			clone(hit.slot);
			break;
		case Part::Preview:
			togglePreview(hit.slot);
			break;
		case Part::Delete:
			deleteSound(hit.slot);
			break;
		case Part::NewSound:
			newSound();
			break;
		case Part::Variation:
			activateVariation(hit.slot, hit.index);
			break;
		case Part::VariationStore:
			storeVariationAt(hit.slot);
			break;
		case Part::VariationForget:
			forgetVariationAt(hit.slot, hit.index);
			break;
		case Part::Card:
			selection_.select(hit.slot);
			// AND THE PRESS IS ARMED, not yet a drag. See kDragThreshold: a card is 38px tall and the
			// three verbs on it are eleven pixels square, so the reorder gesture may not begin until the
			// pointer has said it means to move. Armed only from the card body, so every verb the hit
			// test answered above still gets its plain click.
			armed_ = hit.slot;
			anchorY_ = e.position.y;
			break;
		case Part::None:
			break;
		}
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!armed_.has_value()) {
			return;
		}
		if (!dragging_ && std::abs(e.position.y - anchorY_) < kDragThreshold) {
			return;
		}
		dragging_ = true;
		drop_ = dropGapAt(e.position);
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		const std::optional<size_t> from = armed_;
		const std::optional<size_t> to = drop_;
		const bool moved = dragging_;
		armed_.reset();
		drop_.reset();
		dragging_ = false;
		if (moved && from.has_value() && to.has_value()) {
			// THE GAP BECOMES A DESTINATION, and the two gaps either side of the dragged row answer
			// nullopt — a drop where the sound already is moves nothing rather than moving by zero,
			// which would still spend an undo step and still announce itself. See drop_gap.h.
			const std::vector<size_t> lane = occupiedLane();
			const auto at = std::find(lane.begin(), lane.end(), *from);
			if (at != lane.end()) {
				const auto fromRow = static_cast<size_t>(at - lane.begin());
				const std::optional<size_t> row = rowForGap(fromRow, *to, lane.size());
				if (row.has_value() && *row < lane.size()) {
					reorder(*from, lane[*row]);
				}
			}
		}
		redraw();
	};

	// DRAGGING OUT IS THE CANCEL. The pointer leaving the rail drops the target rather than the whole
	// gesture, so a hand that wanders out and back finds the drag still in its hand — and a release
	// outside moves nothing, because there is no target to move to.
	onMouseExit() = [this](const visage::MouseEvent&) {
		if (drop_.has_value()) {
			drop_.reset();
			redraw();
		}
	};
}

//----------//
// Geometry //
//----------//

size_t SoundStack::variationRows(size_t slot) const {
	if (slot >= kMaxSounds) {
		return 0;
	}
	// ONE ROW FOR A SOUND THAT HAS STORED NOTHING, because it still has an exploration — the one
	// its parameters and state channels hold (V20: the row shows the stance's HOME, not a place) —
	// and that implicit row is the active record its edits mutate. So the rail is unchanged until
	// somebody stores a stance, and the second level appears exactly where the mock draws it.
	const SoundVariations::List& list = bridge_.soundVariations().sound[slot];
	return list.empty() ? 1 : list.count;
}

RideAlongValues SoundStack::liveRideAlong(size_t slot) const {
	// The live slot's ride-along values — THE EXPLORE BOX (V20), read down the same roads
	// readIdentity reads, minus the vernier (a tray knob, outside the box) plus width. A future
	// Explore-box control joins the family by joining this read, the write below, and the wire.
	const size_t at = std::min(slot, kMaxSounds - 1);
	const AnchorPoint drift = bridge_.soundTravel().offset[at];
	RideAlongValues values { bridge_.value(static_cast<clap_id>(soundParam(at, SoundField::Width))),
		     bridge_.value(static_cast<clap_id>(soundParam(at, SoundField::Zone))),
		     bridge_.value(static_cast<clap_id>(soundParam(at, SoundField::Offset))),
		     bridge_.soundGamma().base[at],
		     drift.x,
		     drift.y,
		     bridge_.soundOrientations().pin[at],
		     bridge_.soundResidue().banked[at],
		     bridge_.soundTravel().orbit[at].radius,
		     bridge_.soundTravel().orbit[at].theta };
	// AND THE SOUND'S KNOBS AND DRAWER, off the one list a clone copies too (perSoundKnobParam).
	for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
		values.knobs[k] = bridge_.value(static_cast<clap_id>(perSoundKnobParam(at, k)));
	}
	values.curve = bridge_.spectralCurve(at);
	return values;
}

RideAlongValues SoundStack::variationValuesAt(size_t slot, size_t index) const {
	// The stance's stored ride-alongs (V18), with variationAt's own empty-list rule: a sound that
	// has stored nothing has the one stance its parameters hold, values included.
	if (slot >= kMaxSounds) {
		return {};
	}
	const SoundVariations::List& list = bridge_.soundVariations().sound[slot];
	if (list.empty()) {
		return liveRideAlong(slot);
	}
	// The active stance is the live record for its values too — see variationAt.
	if (index == list.active) {
		return liveRideAlong(slot);
	}
	return index < list.count ? list.values[index] : RideAlongValues {};
}

double SoundStack::variationHome(size_t slot, size_t index) const {
	// Where the stance lives on the EXPLORE axis — a stance is a stored exploration since V20, so
	// its row is named by the home it recalls: the stored zone, offset and gamma base, plus the
	// live vernier and global gamma halves the stance deliberately does not carry (tray knobs; the
	// row should agree with the chart, which adds them too).
	const RideAlongValues values = variationValuesAt(slot, index);
	return values.zone / 1023.0 + values.offset + values.gammaBase
	       + bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Gamma)))
	       + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma));
}

bool SoundStack::activeStance(size_t slot, size_t index) const {
	if (slot >= kMaxSounds || index >= variationRows(slot)) {
		return false;
	}
	// THE LIGHT IS AN ACTIVE-STANCE INDICATOR — V19: turning Width is supposed to mutate the stance,
	// not take you off it. That exposed the shipped
	// snapshot-compare light as a mock divergence: under write-through there is no step-off to be
	// honest about, because editing the sound IS editing its active stance. Which record you are
	// editing is the whole answer, and for a sound that has stored nothing the one drawn row is the
	// implicit active record its parameters hold.
	const SoundVariations::List& list = bridge_.soundVariations().sound[slot];
	return list.empty() ? index == 0 : index == list.active;
}

float SoundStack::slotHeight(size_t slot) const {
	return kCardHeight + kRowGap + static_cast<float>(variationRows(slot)) * kPlaceHeight;
}

bool SoundStack::slotOnScreen(size_t slot) const {
	// THE WHOLE SLOT, ROWS INCLUDED. The cull used to measure the card and one placement row, which is
	// the right number only for a sound that remembers nothing: a sound with four stances is 97px tall
	// and its card is 38 of them, so a scroll that took the card 60px off the top dropped a slot with
	// 37px still showing. The rows vanished a card at a time while the wheel was turning, which is a
	// list that looks broken in exactly the moment somebody is watching it move.
	const visage::Bounds card = cardBounds(slot);
	return card.y() + slotHeight(slot) >= 0.0f && card.y() <= height();
}

float SoundStack::contentHeight() const {
	// A RUNNING SUM, not a multiplication: slots are different heights now, because a sound that
	// remembers four stances draws four rows and the ones below it move down rather than being drawn
	// over.
	float total = kPadTop + kHeadingHeight + kRowGap;
	for (size_t row = 0; row < visibleCount(); ++row) {
		total += slotHeight(visibleSlot(row)) + kRowGap;
	}
	// AND THE ROW THAT MAKES ONE, WHILE THERE IS ONE — with 8 already present it is hidden completely,
	// not dimmed. So it takes no height at the ceiling either; a hidden row that still
	// reserved its space would leave a gap that means nothing.
	return total + (newSoundTarget().has_value() ? kNewRowHeight : 0.0f) + kPadTop;
}

float SoundStack::maxScroll() const {
	return std::max(0.0f, contentHeight() - height());
}

void SoundStack::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped != scroll_) {
		scroll_ = clamped;
		redraw();
	}
}

bool SoundStack::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		return false;
	}
	// One notch moves a whole card, which is the unit the eye is tracking. visage reports the precise
	// delta in notches, positive upward, so the sign flips to move the content the other way.
	setScroll(scroll_ - e.precise_wheel_delta_y * kSlotHeight);
	return true;
}

visage::Bounds SoundStack::cardBounds(size_t slot) const {
	// BY POSITION AMONG THE SLOTS THAT ARE DRAWN, not by slot index. The rail shows consumed slots
	// only, so slot D can be the second card, and a card placed by its slot number would leave gaps
	// where the empty ones used to be.
	float top = kPadTop + kHeadingHeight + kRowGap - scroll_;
	for (size_t row = 0; row < visibleCount(); ++row) {
		const size_t at = visibleSlot(row);
		if (at == slot) {
			break;
		}
		top += slotHeight(at) + kRowGap;
	}
	return { kPadX, top, width() - 2.0f * kPadX, kCardHeight };
}

visage::Bounds SoundStack::newRowBounds() const {
	float top = kPadTop + kHeadingHeight + kRowGap - scroll_;
	for (size_t row = 0; row < visibleCount(); ++row) {
		top += slotHeight(visibleSlot(row)) + kRowGap;
	}
	return { kPadX, top, width() - 2.0f * kPadX, kNewRowHeight };
}

visage::Bounds SoundStack::variationBounds(size_t slot, size_t index) const {
	const visage::Bounds card = cardBounds(slot);
	const float top = card.bottom() + kRowGap + static_cast<float>(index) * kPlaceHeight;
	return { card.x(), top, card.width(), kPlaceHeight };
}

visage::Bounds SoundStack::variationForgetBounds(size_t slot, size_t index) const {
	const visage::Bounds row = variationBounds(slot, index);
	return { row.right() - kCardPadX - kRowActSize, row.y() + (row.height() - kRowActSize) * 0.5f,
		     kRowActSize, kRowActSize };
}

visage::Bounds SoundStack::variationStoreBounds(size_t slot, size_t index) const {
	const visage::Bounds forget = variationForgetBounds(slot, index);
	return { forget.x() - kRowActGap - kRowActSize, forget.y(), kRowActSize, kRowActSize };
}

visage::Bounds SoundStack::deleteBounds(size_t slot) const {
	// RIGHTMOST, PAST THE SWITCH — the end of the line. The one verb here that destroys must not sit
	// between two that are safe, so it takes the position the hand can learn as "last".
	const visage::Bounds card = cardBounds(slot);
	const float lineCentre = card.y() + kCardPadY + kCardReadoutSize * 0.5f;
	return { card.right() - kCardPadX - kDeleteSize, lineCentre - kDeleteSize * 0.5f, kDeleteSize,
		     kDeleteSize };
}

visage::Bounds SoundStack::switchBounds(size_t slot) const {
	const visage::Bounds card = cardBounds(slot);
	// Right-aligned inside the card's padding, on the first line's centre — the mock's `.acts` sits
	// at `margin-left:auto` in the flex row the zone readout starts. Left of the delete ✕ since that
	// verb took the edge.
	const float lineCentre = card.y() + kCardPadY + kCardReadoutSize * 0.5f;
	return { card.right() - kCardPadX - kDeleteSize - kDeleteGap - kSwitchWidth,
		     lineCentre - kSwitchHeight * 0.5f, kSwitchWidth, kSwitchHeight };
}

visage::Bounds SoundStack::cloneBounds(size_t slot) const {
	// Left of the switch, on the same line: the mock's `.acts` holds ⧉ and the pill side by side, in
	// that order, and this is that row with one of its two verbs built.
	const visage::Bounds pill = switchBounds(slot);
	const float lineCentre = pill.y() + pill.height() * 0.5f;
	return { pill.x() - kCloneGap - kCloneSize, lineCentre - kCloneSize * 0.5f, kCloneSize,
		     kCloneSize };
}

visage::Bounds SoundStack::previewBounds(size_t slot) const {
	// Left of the clone, on the same line. The `.acts` row reads switch-clone-preview right to left,
	// which puts the two verbs that MAKE something (clone) and CHANGE nothing (preview) either side of
	// each other and the switch — the one control that changes the patch — hard against the edge.
	const visage::Bounds copy = cloneBounds(slot);
	const float lineCentre = copy.y() + copy.height() * 0.5f;
	return { copy.x() - kPreviewGap - kPreviewSize, lineCentre - kPreviewSize * 0.5f, kPreviewSize,
		     kPreviewSize };
}

SoundStack::Hit SoundStack::hitAt(visage::Point position) const {
	if (position.x < 0.0f || position.x >= width() || position.y < 0.0f || position.y >= height()) {
		return {};
	}
	for (size_t row = 0; row < visibleCount(); ++row) {
		const size_t slot = visibleSlot(row);
		// THE PREVIEW BUTTON FIRST, padded like its neighbours and stopping short of them: three small
		// verbs on one line, and a press that lands between two of them must answer to one rather than
		// falling through to the card and selecting the sound instead.
		visage::Bounds listen = previewBounds(slot);
		listen = { listen.x() - kCardPadX * 0.5f, listen.y() - kCardPadY,
			       listen.width() + kPreviewGap * 0.5f, listen.height() + 2.0f * kCardPadY };
		if (listen.contains(position)) {
			return { Part::Preview, slot };
		}
		// THE CLONE BUTTON NEXT, padded the same way and for the same reason: it is eleven pixels square
		// on a card that answers every other pixel, so a press a hair off it would select the sound
		// instead — a different verb with no visible cause.
		visage::Bounds copy = cloneBounds(slot);
		copy = { copy.x() - kCardPadX * 0.5f, copy.y() - kCardPadY, copy.width() + kCloneGap * 0.5f,
			     copy.height() + 2.0f * kCardPadY };
		// A KIT HAS NO CLONE AND NO DELETE (hidden, not greyed): its eight pads are always present.
		const bool kit = kitMode();
		if (!kit && copy.contains(position)) {
			return { Part::Clone, slot };
		}
		// THE DELETE ✕ BEFORE THE SWITCH — it owns the card's right edge, and a press between the two
		// answers to the one checked first, so the destructive verb's border is deterministic rather
		// than shared.
		visage::Bounds gone = deleteBounds(slot);
		gone = { gone.x() - kDeleteGap * 0.5f, gone.y() - kCardPadY,
			     gone.width() + kDeleteGap * 0.5f + kCardPadX * 0.5f, gone.height() + 2.0f * kCardPadY };
		if (!kit && gone.contains(position)) {
			return { Part::Delete, slot };
		}
		// THE SWITCH, and generously: it is 18x10 inside a card that answers every other pixel,
		// so a press a few pixels off it would select the sound instead of switching it — a different
		// verb with no visible cause. The pad is the card's own, so the grab area never leaves the card.
		visage::Bounds pill = switchBounds(slot);
		pill = { pill.x() - kCardPadX * 0.5f, pill.y() - kCardPadY, pill.width() + kCardPadX,
			     pill.height() + 2.0f * kCardPadY };
		if (pill.contains(position)) {
			return { Part::Enable, slot };
		}
		// THE VARIATION ROWS, each with its own two verbs. Checked before the card so a press on a row's
		// buttons is not swallowed by the sound underneath them.
		for (size_t index = 0; index < variationRows(slot); ++index) {
			const visage::Bounds row = variationBounds(slot, index);
			if (!row.contains(position)) {
				continue;
			}
			if (variationForgetBounds(slot, index).contains(position)) {
				return { Part::VariationForget, slot, index };
			}
			if (variationStoreBounds(slot, index).contains(position)) {
				return { Part::VariationStore, slot, index };
			}
			// The rest of the row recalls the stance — the mock's radio light, which is the row itself.
			return { Part::Variation, slot, index };
		}

		// The card is the sound. Its rows were answered above, so what is left is the card proper.
		const visage::Bounds card = cardBounds(slot);
		if (card.contains(position)) {
			return { Part::Card, slot };
		}
	}
	// AND THE ROW THAT MAKES A SOUND, after the last card — the only control here that is not about a
	// sound that already exists, and only while there is a slot for it to use. A hidden control that
	// still answered a press would be worse than a dimmed one: nothing on screen would explain what
	// just happened, or failed to.
	if (newSoundTarget().has_value() && newRowBounds().contains(position)) {
		return { Part::NewSound, 0 };
	}
	return {};
}

std::optional<size_t> SoundStack::dropGapAt(visage::Point position) const {
	const size_t rows = visibleCount();
	if (rows == 0) {
		return std::nullopt;
	}
	// OUTSIDE THE RAIL IS NO TARGET AT ALL — that is the cancel. Inside it, a point above the first
	// card or below the last one is the END of the list rather than nothing: the hand aimed past the
	// list because that is where it wanted to go, and a GAP can say so where the slot this used to
	// return could not.
	if (position.x < 0.0f || position.x >= width() || position.y < 0.0f || position.y >= height()) {
		return std::nullopt;
	}
	// THE WHOLE SLOT IS THE ROW, stance rows and all, because a drop is aimed past a SOUND and a
	// sound's rows are part of the thing being aimed past. Handed to the shared arithmetic, which
	// answers by each row's midpoint — so the top half of a card means "above this one" and the
	// bottom half means "below", rather than the whole card meaning one thing.
	std::vector<RowSpan> spans;
	spans.reserve(rows);
	for (size_t row = 0; row < rows; ++row) {
		const size_t slot = visibleSlot(row);
		const visage::Bounds card = cardBounds(slot);
		spans.push_back({ card.y(), card.y() + slotHeight(slot) });
	}
	return gapAlong(position.y, spans);
}

visage::Bounds SoundStack::gapBounds(size_t gap) const {
	const size_t rows = visibleCount();
	if (gap > rows) {
		return {};
	}
	// A BUMPER SITS IN THE GAP THE CARDS LEAVE — kRowGap of space between two of them — rather than
	// on either card. Drawn as a band across the rail's inner width so it reads as a place a thing
	// goes, not as a property of the card above or below it.
	const float thickness = 2.0f;
	float y = 0.0f;
	if (gap < rows) {
		y = cardBounds(visibleSlot(gap)).y() - kRowGap * 0.5f - thickness * 0.5f;
	}
	else {
		const size_t last = visibleSlot(rows - 1);
		y = cardBounds(last).y() + slotHeight(last) + kRowGap * 0.5f - thickness * 0.5f;
	}
	return { kPadX, y, width() - 2.0f * kPadX, thickness };
}

std::optional<size_t> SoundStack::spliceRoom(size_t slot) const {
	if (slot >= kMaxSounds) {
		return std::nullopt;
	}
	// THE NEAREST EMPTY SLOT, looking DOWN the list first. Nearest because every slot between it and
	// the source has to move, and a splice that shifted six sounds when it could have shifted one is
	// six sounds' worth of parameter traffic and six letters re-badged for nothing. Downward first
	// because that is where a clone goes when both directions are equally close, which keeps the
	// common case — a rail with room at the end — moving nothing at all.
	for (size_t at = slot + 1; at < kMaxSounds; ++at) {
		if (isEmpty(at)) {
			return at;
		}
	}
	for (size_t at = slot; at-- > 0;) {
		if (isEmpty(at)) {
			return at;
		}
	}
	return std::nullopt;
}

//-------//
// State //
//-------//

AnchorPoint SoundStack::livePlacement(size_t slot) const {
	return { bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX))),
		     bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::PlaceY))) };
}

uint32_t SoundStack::seedHue(size_t slot) const {
	// THE COLOUR THIS SOUND WEARS, read from the sound rather than computed from the row it is in.
	// That is the whole of "reordering retains the original colour": the palette still
	// lives here, because a colour is a design token, and WHICH entry a sound wears is state that
	// travels with it (sound_hues.h).
	return kSeedHues[hueIndexOf(bridge_.soundHues(), slot)];
}

bool SoundStack::enabled(size_t slot) const {
	const double value = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Enable)));
	return steppedIndex(value, kEnableNames.size()) != 0;
}


bool SoundStack::kitMode() const {
	return oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType))) == OscType::Drum
	       && steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	              == static_cast<size_t>(DrumMode::Kit);
}

bool SoundStack::padMuted(size_t slot) const {
	return std::lround(bridge_.value(static_cast<clap_id>(drumParam(slot, DrumControl::Mute)))) != 0;
}

void SoundStack::togglePadMute(size_t slot) {
	// IN A KIT THE CARD'S SWITCH IS ITS PAD'S MUTE: every pad is present, so the switch no
	// longer says whether the sound exists; it says whether its pad sounds.
	const bool muted = padMuted(slot);
	(void)writeParamAsEdit(bridge_, static_cast<clap_id>(drumParam(slot, DrumControl::Mute)), muted ? 0.0 : 1.0,
	                       muted ? "Pad on" : "Pad muted");
	if (notifier_ != nullptr) {
		notifier_->say(std::string("Pad ") + static_cast<char>('A' + slot) + (muted ? " on" : " muted"));
	}
	redraw();
}

void SoundStack::layoutLevelFaders() {
	// THE KIT CARDS' LEVEL FADERS: the right half of each card's second line, on a Drum Kit only (hidden otherwise, and dead).
	const bool kit = kitMode();
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		FaderPill& fader = *levelFaders_[slot];
		const bool shown = kit && slotOnScreen(slot);
		if (shown) {
			const visage::Bounds card = cardBounds(slot);
			const float top = card.y() + kCardPadY + kCardReadoutSize + 1.0f;
			fader.setBounds(card.x() + card.width() * 0.5f, top, card.width() * 0.5f - kCardPadX, kCardSubSize + 4.0f);
		}
		if (fader.isVisible() != shown) {
			fader.setVisible(shown);
		}
	}
}

bool SoundStack::updatePulses(int64_t nowMs) {
	// THE HIT PULSE: a pad's count moved since the last frame, so its card lights at the hit's velocity
	// and falls over kPulseMs; under reduced motion it holds lit for that long, then goes out, with no fade between.
	layoutLevelFaders();
	if (!kitMode()) {
		return false;
	}
	const PadHits hits = bridge_.padHits();
	bool moved = false;
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (hits.count[pad] != pulseSeen_[pad]) {
			pulseSeen_[pad] = hits.count[pad];
			pulseLevel_[pad] = std::clamp(hits.velocity[pad], 0.0f, 1.0f);
			pulseAt_[pad] = nowMs;
			moved = true;
		}
		if (pulseLevel_[pad] > 0.0f) {
			moved = true;
		}
	}
	if (moved) {
		redraw();
	}
	pulseNow_ = nowMs;
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (static_cast<float>(nowMs - pulseAt_[pad]) >= kPulseMs) {
			pulseLevel_[pad] = 0.0f; // spent: no more frames owed for it
		}
	}
	return moved;
}

float SoundStack::pulseOf(size_t slot) const {
	const float age = static_cast<float>(pulseNow_ - pulseAt_[slot]);
	if (pulseLevel_[slot] <= 0.0f || age >= kPulseMs) {
		return 0.0f;
	}
	if (reducedMotionRequested()) {
		return pulseLevel_[slot]; // lit, still, then out
	}
	return pulseLevel_[slot] * std::exp(-3.0f * age / kPulseMs);
}
void SoundStack::toggleEnable(size_t slot) {
	const auto id = static_cast<clap_id>(soundParam(slot, SoundField::Enable));
	const bool on = enabled(slot);
	// One complete edit, bracketed, exactly as SegmentedControl does it — switching a sound in or out
	// of the constellation is one undoable step, not an unmatched value — in the host's undo AND the
	// editor's, which until backlog JB this claimed and did not record.
	(void)writeParamAsEdit(bridge_, id, on ? 0.0 : 1.0, on ? "Sound off" : "Sound on");
	// Switching a sound ON also selects it: you turned it on to work on it, and leaving the selection
	// behind on a different card would make the next Explore gesture edit the wrong sound.
	if (!on) {
		selection_.select(slot);
	}
	if (notifier_ != nullptr) {
		notifier_->say(std::string("Sound ") + static_cast<char>('A' + slot) + (on ? " off" : " on"));
	}
	redraw();
}

namespace {

/// ONE SOUND'S STANCES AND WHERE IT IS STANDING, as an undoable act.
///
/// A record type written OUTSIDE edit_history.h, which is the point of that file's shape: a new verb
/// brings its own record and the history does not have to learn about it. It restores what the three
/// variation verbs between them change — the stored list, and the placement parameters that a recall
/// writes — and it captures values plus the bridge, never a widget, so walking it is safe whatever has
/// happened to the rail since.
/// Write one full ride-along set into a slot's LIVE state — the recall road (V20): the same
/// channels writeIdentity drives, minus the vernier (outside the box), plus width, and never the
/// place. Guarded per channel like every write in this file, so restoring values a slot already
/// holds spends no gesture.
void writeRideAlong(ParamBridge& bridge, size_t slot, const RideAlongValues& values) {
	const size_t at = std::min(slot, kMaxSounds - 1);
	const auto write = [&](SoundField field, double value) {
		const auto id = static_cast<clap_id>(soundParam(at, field));
		if (bridge.value(id) == value) {
			return;
		}
		bridge.beginGesture(id);
		bridge.setValue(id, value);
		bridge.endGesture(id);
	};
	write(SoundField::Zone, values.zone);
	write(SoundField::Offset, values.offset);
	write(SoundField::Width, values.width);
	if (bridge.soundGamma().base[at] != values.gammaBase) {
		bridge.beginSoundGammaGesture();
		bridge.setSoundGamma(at, values.gammaBase);
		bridge.endSoundGammaGesture();
	}
	const AnchorPoint drift = bridge.soundTravel().offset[at];
	if (drift.x != values.driftX || drift.y != values.driftY) {
		bridge.beginSoundTravelGesture();
		bridge.setSoundTravel(at, { values.driftX, values.driftY });
		bridge.endSoundTravelGesture();
	}
	const PolarOffset orbit { values.orbitRadius, values.orbitTheta };
	if (bridge.soundTravel().orbit[at] != orbit) {
		bridge.beginSoundTravelGesture();
		bridge.setSoundOrbit(at, orbit);
		bridge.endSoundTravelGesture();
	}
	// The plane and its residue in one breath (A26): a turn restored without what it banked
	// restores a different sound.
	if (!(bridge.soundOrientations().pin[at] == values.plane)) {
		bridge.beginSoundOrientationGesture();
		bridge.setSoundOrientation(at, values.plane);
		bridge.endSoundOrientationGesture();
	}
	if (!(bridge.soundResidue().banked[at] == values.residue)) {
		bridge.beginSoundOrientationGesture();
		bridge.setSoundResidue(at, values.residue);
		bridge.endSoundOrientationGesture();
	}
	// AND THE SOUND'S KNOBS AND DRAWER (a stance carries them), each guarded like the rest, and the curve
	// when it moved (setSpectralCurve is not an edit for a curve that did not).
	for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
		const auto id = static_cast<clap_id>(perSoundKnobParam(at, k));
		if (bridge.value(id) != values.knobs[k]) {
			bridge.beginGesture(id);
			bridge.setValue(id, values.knobs[k]);
			bridge.endGesture(id);
		}
	}
	bridge.setSpectralCurve(at, values.curve);
}

class StanceEdit : public feathers::ui::EditRecord {
public:
	struct State {
		SoundVariations::List list;
		/// The LIVE ride-along values at this state — the full explore box since V20 (place left
		/// the stance, so no place travels here at all). What a recall writes and what an undo of
		/// one restores.
		RideAlongValues values;

		friend bool operator==(const State&, const State&) = default;
	};

	/// `follows` says whether recording this edit should point the chart at the sound (V19 polish:
	/// the terrain re-centres when the variant switches): TRUE for the verbs that
	/// MOVE the sound between records — a recall, and a forget that deleted the record you occupied
	/// — and false for a store, which keeps you standing exactly where you were.
	StanceEdit(ParamBridge& bridge, size_t slot, State before, State after, const char* verb,
	           bool follows = false)
	    : bridge_(bridge), slot_(slot), before_(before), after_(after), verb_(verb),
	      follows_(follows) {}

	void undo() const override { restore(before_); }
	void redo() const override { restore(after_); }
	const char* verb() const override { return verb_; }
	std::optional<size_t> slot() const override { return slot_; }
	bool followsOnRecord() const override { return follows_; }

private:
	void restore(const State& state) const {
		bridge_.beginSoundVariationsGesture();
		bridge_.setSoundVariations(slot_, state.list);
		bridge_.endSoundVariationsGesture();
		// The whole explore box (V20) — and nothing else: place is not the stance's to restore.
		writeRideAlong(bridge_, slot_, state.values);
	}

	ParamBridge& bridge_;
	size_t slot_ = 0;
	State before_;
	State after_;
	const char* verb_ = "stance";
	bool follows_ = false;
};

/// SEVERAL SLOTS AT ONCE, AS ONE ACT — the shape the verbs that MOVE sounds between slots need, and
/// the one shape `SlotEdit` cannot take. A clone that splices and a drag that reorders both rewrite a
/// run of slots, and a hand that walked one of those back a slot at a time would be walking through
/// states the list was never in.
///
/// NO HISTORY REMAPPING, and this is worth stating because the mock does remap and we deliberately do
/// not. Its `reorderSound()` rewrites every stored entry's sound index — it has to, because its
/// history is a list of edits addressed by list position and a splice moves those positions. Ours is
/// a strict stack: an older record can only ever be applied AFTER this one has been undone, and
/// undoing this one restores the very slots those older records name. So their addresses are correct
/// again by the time anything can reach them, and remapping would be a second mechanism that could
/// only disagree with the first.
///
/// The FOCUS is which sound the surface should point at afterwards, and it differs by direction: redo
/// leaves the moved sound at its destination, undo leaves it where it started. Held as a mutable
/// pick rather than as two accessors so `EditRecord::slot()` — which is what an observer asks — keeps
/// answering the one honest question, "which sound was this about", in the direction just walked.
class OrderEdit : public feathers::ui::EditRecord {
public:
	struct Step {
		size_t slot = 0;
		SlotState before;
		SlotState after;
	};

	/// AND THE PATCH'S LIKES BEFORE AND AFTER, when the act moved them (a clone copies its source's, backlog PC). The
	/// likes are not a parameter and not in SlotState — they are keyed by slot in one patch-wide list — so the record
	/// carries the whole list on each side, which a strict stack makes exact: by the time this record can be walked,
	/// every later edit to the likes has been walked back first.
	OrderEdit(ParamBridge& bridge, std::vector<Step> steps, size_t focusBefore, size_t focusAfter,
	          const char* verb, std::optional<std::pair<TerrainLikes, TerrainLikes>> likes = std::nullopt)
	    : bridge_(bridge), steps_(std::move(steps)), focusBefore_(focusBefore),
	      focusAfter_(focusAfter), focus_(focusAfter), verb_(verb), likes_(std::move(likes)) {}

	void undo() const override {
		for (const Step& step : steps_) {
			writeSlotState(bridge_, step.slot, step.before);
		}
		if (likes_.has_value()) {
			bridge_.setTerrainLikes(likes_->first);
		}
		focus_ = focusBefore_;
	}
	void redo() const override {
		for (const Step& step : steps_) {
			writeSlotState(bridge_, step.slot, step.after);
		}
		if (likes_.has_value()) {
			bridge_.setTerrainLikes(likes_->second);
		}
		focus_ = focusAfter_;
	}
	const char* verb() const override { return verb_; }
	std::optional<size_t> slot() const override { return focus_; }

	bool empty() const noexcept { return steps_.empty(); }

private:
	/// THE BRIDGE AND VALUES, never a widget — edit_history.h's rule, and the reason a record can be
	/// walked long after the rail that made it has been destroyed.
	ParamBridge& bridge_;
	std::vector<Step> steps_;
	size_t focusBefore_ = 0;
	size_t focusAfter_ = 0;
	mutable size_t focus_ = 0;
	const char* verb_ = "order";
	std::optional<std::pair<TerrainLikes, TerrainLikes>> likes_;
};

/// THE LANE A SPLICE MOVES ALONG: the slots the list occupies, in order, plus whichever empty slot
/// the act is about to consume. Everything a reorder or a clone-next does is "write this sequence of
/// states into this sequence of slots".
///
/// EMPTY SLOTS ARE NOT PART OF IT, and that is a correction rather than a simplification. Shifting
/// them along the way seemed free — an empty slot moved is still empty — and it is not: emptiness is
/// measured against the slot's OWN defaults, and the placement defaults are the designed ring, a
/// different point per slot. Slot 2's empty state written into slot 3 is a slot standing somewhere
/// slot 3 was never born standing, which reads as authored, which puts a card in the rail for a sound
/// nobody made. A case caught exactly that. So the gaps stay where they are and only the sounds move.
std::vector<OrderEdit::Step> laneSteps(const ParamBridge& bridge, const std::vector<size_t>& lane,
                                       const std::vector<SlotState>& states) {
	std::vector<OrderEdit::Step> steps;
	for (size_t at = 0; at < lane.size() && at < states.size(); ++at) {
		SlotState before = readSlotState(bridge, lane[at]);
		if (before == states[at]) {
			continue;
		}
		steps.push_back({ lane[at], std::move(before), states[at] });
	}
	return steps;
}

/// THE LIKES FOLLOW THEIR SOUNDS (backlog PC). A like is keyed by the SLOT it was made on (terrain_likes.h), so every
/// verb that moves sounds between slots re-keys the patch's likes with it: a like on a slot in `movedTo` goes to where
/// that sound now stands, a like on `drop` (a deleted sound's, or an orphan on a slot a splice spends) is removed, and
/// every other like stays as it was. The order they were made in is kept, since the newest like of a sound is its pin.
using SlotMoves = std::array<std::optional<size_t>, kMaxSounds>;
TerrainLikes rekeyLikes(const TerrainLikes& likes, const SlotMoves& movedTo, std::optional<size_t> drop) {
	TerrainLikes out;
	for (const TerrainLike& like : likes.likes) {
		const size_t was = like.slot;
		if (was < kMaxSounds && movedTo[was].has_value()) {
			TerrainLike moved = like;
			moved.slot = static_cast<uint16_t>(*movedTo[was]);
			out.likes.push_back(moved);
		}
		else if (!drop.has_value() || was != *drop) {
			out.likes.push_back(like);
		}
	}
	return out;
}

/// A slot's identity as the PARAMETER TABLE declares it — what is in a slot nobody has touched.
///
/// Read from the table rather than written out here, so a default that changes changes this with it.
/// Used for one question only: did a clone land on an empty slot or on a sound somebody parked?
SoundIdentity untouchedIdentity(size_t slot) {
	const auto def = [&](SoundField field) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(soundParam(slot, field)));
		return desc != nullptr ? desc->def : 0.0;
	};
	SoundIdentity fresh;
	fresh.zone = def(SoundField::Zone);
	fresh.offset = def(SoundField::Offset);
	fresh.gamma = def(SoundField::Gamma);
	return fresh;
}

} // namespace

bool SoundStack::isEmpty(size_t slot) const {
	if (slot >= kMaxSounds) {
		return false;
	}
	// A DRUM KIT'S EIGHT ARE ALWAYS PRESENT: every sound is a pad, so none is an empty slot, which is what
	// hides the new-sound row and gives a clone nowhere to land.
	if (kitMode()) {
		return false;
	}
	if (enabled(slot)) {
		return false;
	}
	if (!(readIdentity(bridge_, slot) == untouchedIdentity(slot))) {
		return false;
	}
	// THE PLACEMENT COUNTS TOO. It is not part of the identity (A34 — where you play a sound from is
	// not what it is), but it IS something a hand can author, and a slot somebody has moved in the
	// circle is a slot somebody has used. The defaults are the designed ring, so a slot nobody has
	// touched matches its own. Width counts by the same was-authored argument — it is a RIDE-ALONG
	// since V18, not placement, but consumption never asked which category a hand authored in: a
	// slot somebody has widened is a slot somebody has used.
	const ParamDesc* placeX = findParam(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX)));
	const ParamDesc* placeY = findParam(static_cast<clap_id>(soundParam(slot, SoundField::PlaceY)));
	const ParamDesc* width = findParam(static_cast<clap_id>(soundParam(slot, SoundField::Width)));
	const AnchorPoint live = livePlacement(slot);
	if (placeX == nullptr || placeY == nullptr || live.x != placeX->def || live.y != placeY->def) {
		return false;
	}
	if (width == nullptr
	    || bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Width))) != width->def) {
		return false;
	}
	return bridge_.soundVariations().sound[slot].empty();
}

size_t SoundStack::visibleCount() const {
	size_t count = 0;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (!isEmpty(slot)) {
			++count;
		}
	}
	return count;
}

size_t SoundStack::visibleSlot(size_t position) const {
	size_t seen = 0;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (isEmpty(slot)) {
			continue;
		}
		if (seen == position) {
			return slot;
		}
		++seen;
	}
	return kMaxSounds;
}

namespace {

/// Where a new sound goes: the emptiest part of the circle, which is the design's farthest-point rule
/// and the mock's `farthestPoint()`. Deterministic — a lattice of candidates scored by distance to the
/// nearest sound already placed — because a spawn nobody can predict is a spawn nobody can undo by
/// hand, and because a test that cannot say where it landed can only say that it landed.
AnchorPoint farthestPointFrom(const std::vector<AnchorPoint>& taken) {
	if (taken.empty()) {
		return { 0.0, 0.0 };
	}
	AnchorPoint best { 0.0, 0.0 };
	double bestDistance = -1.0;
	constexpr int kSteps = 24;
	for (int iy = 0; iy <= kSteps; ++iy) {
		for (int ix = 0; ix <= kSteps; ++ix) {
			const double x = -0.85 + 1.7 * static_cast<double>(ix) / kSteps;
			const double y = -0.85 + 1.7 * static_cast<double>(iy) / kSteps;
			if (x * x + y * y > 0.85 * 0.85) {
				continue; // outside the disk the circle draws
			}
			double nearest = 1e9;
			for (const AnchorPoint& at : taken) {
				const double dx = at.x - x;
				const double dy = at.y - y;
				nearest = std::min(nearest, dx * dx + dy * dy);
			}
			if (nearest > bestDistance) {
				bestDistance = nearest;
				best = { x, y };
			}
		}
	}
	return best;
}

} // namespace

std::optional<size_t> SoundStack::newSoundTarget() const {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (isEmpty(slot)) {
			return slot;
		}
	}
	return std::nullopt;
}

void SoundStack::newSound() {
	const std::optional<size_t> target = newSoundTarget();
	if (!target.has_value()) {
		// NO TRAY LINE HERE, deliberately, and this is the difference between a refusal and a
		// precondition: the row hides at the ceiling, so there is no press to answer. A message nobody
		// can trigger is dead text — the guard stays because a caller that is not a press can still
		// arrive here.
		return;
	}
	const SlotState before = readSlotState(bridge_, *target);

	// A ROLLED IDENTITY, as the mock mints one (`r()*ZONES`, `r()`) rather than a sound born at the
	// origin: a new sound should be somewhere worth listening to, and Explore is how you move it from
	// there. Seeded by the slot so the same empty slot always mints the same sound — a spawn that
	// changed under an undo and redo would be a verb nobody could walk back.
	uint32_t seed = 0x9E3779B9u + static_cast<uint32_t>(*target) * 0x85EBCA6Bu;
	const auto roll = [&seed]() {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		return static_cast<double>(seed & 0xFFFFFFu) / static_cast<double>(0x1000000u);
	};
	const ParamDesc* zone = findParam(static_cast<clap_id>(soundParam(*target, SoundField::Zone)));
	SlotState after = before;
	after.enabled = true;
	// THE LOWEST COLOUR NOBODY IS WEARING, the same rule a clone follows — a sound that arrived
	// wearing a colour already on screen would be two sounds with one name.
	after.hue = lowestUnusedHue(bridge_.soundHues(), occupiedLane());
	after.identity.zone = zone != nullptr ? std::round(roll() * zone->max) : 0.0;
	after.identity.offset = roll();
	writeSlotState(bridge_, *target, after);

	// AND A PLACE OF ITS OWN, in the emptiest part of the circle — the design's farthest-point spawn,
	// so a new sound claims territory rather than crowding one that is already there.
	std::vector<AnchorPoint> taken;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (slot != *target && !isEmpty(slot)) {
			taken.push_back(livePlacement(slot));
		}
	}
	const AnchorPoint at = farthestPointFrom(taken);
	const auto place = [&](SoundField field, double value) {
		const auto id = static_cast<clap_id>(soundParam(*target, field));
		if (bridge_.value(id) == value) {
			return;
		}
		bridge_.beginGesture(id);
		bridge_.setValue(id, value);
		bridge_.endGesture(id);
	};
	place(SoundField::PlaceX, at.x);
	place(SoundField::PlaceY, at.y);

	// RECORDED AFTER THE PLACEMENT, and read back off the bridge rather than assembled by hand, so the
	// entry holds every part of what this verb did. An entry built before the placing would restore an
	// identity into a slot still standing where the new sound was put — which leaves the slot looking
	// used, and the rail drawing a sound nobody made.
	recordSlot(history_, bridge_, *target, before, readSlotState(bridge_, *target), "new sound");
	selection_.select(*target);
	if (notifier_ != nullptr) {
		notifier_->say(std::string("Sound ") + static_cast<char>('A' + *target) + " added");
	}
	redraw();
}

void SoundStack::deleteSound(size_t slot) {
	// The rail never offers the verb on an empty slot — an empty slot draws no card — but the guard
	// stays for a caller that is not a press.
	if (slot >= kMaxSounds || isEmpty(slot)) {
		return;
	}
	const SlotState before = readSlotState(bridge_, slot);

	// DELETED IS NEVER-USED. The after-state is the parameter table's own defaults — the exact values
	// isEmpty reads — so a deleted slot and a fresh one are ONE state rather than two that look alike,
	// and the "+ New sound" row returns by the same derivation that hid it.
	SlotState after;
	after.identity = untouchedIdentity(slot);
	after.enabled = false;
	// The whole ride-along family to its defaults — no stances, every value member at its table
	// default. One reset, so a member added to the family cannot be left behind by a delete.
	const ParamDesc* placeX = findParam(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX)));
	const ParamDesc* placeY = findParam(static_cast<clap_id>(soundParam(slot, SoundField::PlaceY)));
	const ParamDesc* width = findParam(static_cast<clap_id>(soundParam(slot, SoundField::Width)));
	after.rideAlong = { width != nullptr ? width->def : 0.0, {} };
	after.place = { placeX != nullptr ? placeX->def : 0.0, placeY != nullptr ? placeY->def : 0.0 };
	// AND THE COLOUR GOES BACK TO THE SLOT'S OWN, which keeps "deleted is never-used" exactly true:
	// a freed slot is bit-for-bit the slot nobody ever touched, colour included. It costs nothing to
	// get right and it is what stops a deleted slot from holding a colour hostage — `lowestUnusedHue`
	// only counts the slots that hold a sound, so the colour is already free either way. (The width
	// reset lives in the ride-along family line above.)
	after.hue = static_cast<uint8_t>(slot);
	// And the sound's knobs back to the table's, as every other part of a deleted sound goes back.
	after.knobs = untouchedKnobs(slot);

	// A DELETED SOUND CANNOT STAY THE ONE BEING HEARD: preview follows a sound, and the sound is
	// going. Cleared before the write so no frame draws a preview ring around a card that is gone.
	if (previewing(slot)) {
		bridge_.clearPreview();
	}
	// AND THE SOUND'S LIKES GO WITH IT (backlog PC), on every engine: left on the slot they would land on the next sound
	// made there. One group, so one undo puts the sound and its likes back together.
	history_.beginGroup("delete");
	writeSlotState(bridge_, slot, after);
	recordSlot(history_, bridge_, slot, before, readSlotState(bridge_, slot), "delete");
	writeStoredAsEdit<TerrainLikes>(
	    &history_, [this] { return bridge_.terrainLikes(); },
	    [&bridge = bridge_](const TerrainLikes& likes) { bridge.setTerrainLikes(likes); },
	    rekeyLikes(bridge_.terrainLikes(), SlotMoves {}, slot), "delete");
	history_.endGroup();

	// The selection follows the rail: deleting the selected card would leave it pointing at nothing
	// on screen, so it moves to the first sound that still exists. Deleting an unselected card moves
	// nothing, and deleting the last sound leaves the selection where it was — an empty patch has
	// nowhere better to point.
	if (selection_.slot() == slot && visibleCount() > 0) {
		selection_.select(visibleSlot(0));
	}
	if (notifier_ != nullptr) {
		notifier_->say(std::string("Deleted ") + static_cast<char>('A' + slot));
	}
	redraw();
}

bool SoundStack::previewing(size_t slot) const {
	const Preview preview = bridge_.preview();
	return preview.active() && preview.slot == slot;
}

bool SoundStack::anyPreviewing() const {
	return bridge_.preview().active();
}

void SoundStack::togglePreview(size_t slot) {
	if (slot >= kMaxSounds) {
		return;
	}
	const bool off = previewing(slot);
	if (off) {
		bridge_.clearPreview();
	}
	else {
		bridge_.setPreview(slot);
		// LISTENING TO A SOUND IS ASKING TO WORK ON IT. Preview follows the selection by design,
		// so pressing it on a card that is not selected would otherwise be a state the rule cannot
		// describe — the sound you hear and the sound Explore edits, different, with nothing said.
		selection_.select(slot);
	}
	if (notifier_ != nullptr) {
		// SAID EVERY TIME, both directions. The rail shows which card is lit, but the tray is what
		// reports a mode change while the eye is on the chart.
		notifier_->say(off ? "Preview off"
		                   : std::string("Preview ") + static_cast<char>('A' + slot) + " alone");
	}
	// EVERY SURFACE THAT DRAWS WHAT IS AUDIBLE, not just this one: the blend circle dims what you
	// cannot hear and the scope draws the sound being made, and neither watches a parameter that
	// moved, because none did.
	previewWatchers_.notify();
}

void SoundStack::activateVariation(size_t slot, size_t index) {
	if (slot >= kMaxSounds || index >= variationRows(slot)) {
		return;
	}
	// SELECTING IS PART OF IT: a click on a row of a sound is a click on that sound, which is what the
	// mock does too — its child rows open the sound in Explore.
	selection_.select(slot);

	const StanceEdit::State before { bridge_.soundVariations().sound[slot], liveRideAlong(slot) };
	SoundVariations::List list = before.list;
	if (list.empty() || index == list.active) {
		// The implicit record, or the record you are already editing: a recall to where you stand is
		// not an edit — under V19 there is no stepped-off state to snap back from.
		redraw();
		return;
	}
	// **REFUSED BEFORE ANYTHING MOVES IF THE STANCE CANNOT BE PUT BACK.** A patch can carry a
	// non-finite ride-along value — `loadSoundVariations` reads them as raw doubles — and every road
	// OUT of a stance guards, so a recall would drop the spoilt member on the floor and land all the
	// others: the light moves, a step is spent, and the sound becomes a mixture of the stance you
	// asked for and the one you were standing in, with nothing anywhere saying so.
	//
	// **AND IT IS REFUSED RATHER THAN SANITISED.** Defaulting the bad member would make the recall
	// succeed at restoring something the player never stored — the same lie in a tidier coat. See
	// `recallable`.
	//
	// **BEFORE THE FIRST BRIDGE WRITE, which is the line that actually matters — measured, because
	// the first draft of this comment claimed more than that.** Moving the refusal past the two lines
	// below that bank the outgoing stance and move `active` changes NOTHING observable and the case
	// stays green: `list` is a local copy, so those edits have not left this function yet. Moving it
	// past `setSoundVariations` reddens it immediately. The ordering constraint is against the
	// BRIDGE, not against the local bookkeeping, and it is written here in the terms the instrument
	// confirmed rather than in the terms that read well.
	if (!recallable(list.values[index])) {
		if (notifier_ != nullptr) {
			char said[64] = {};
			std::snprintf(said, sizeof(said), "Sound %c's stance %zu cannot be restored",
			              static_cast<char>('A' + slot), index + 1);
			notifier_->say(said);
		}
		redraw();
		return;
	}
	// BANK THE OUTGOING STANCE — V19's write-through, settled where it can be observed: while a
	// stance is active its stored entry is dead and the live values are the record, so the moment
	// you switch away is the moment the record's contents become storage again. What is banked is
	// the WHOLE explore box the sound is at now (V20) — hand edits and automation alike, the
	// mock's "the active one simply updates as you play".
	list.values[list.active] = before.values;
	list.active = static_cast<uint8_t>(index);
	// A recall restores the incoming stance WHOLE — the stored exploration (V20): zone, offset, the
	// gamma base, drift, the plane with its residue, and width. NOT the place: a variant switch no
	// longer moves the star, by design (the place leaves the stance).
	const StanceEdit::State after { list, list.values[index] };

	if (before == after) { return; }

	bridge_.beginSoundVariationsGesture();
	bridge_.setSoundVariations(slot, list);
	bridge_.endSoundVariationsGesture();
	writeRideAlong(bridge_, slot, after.values);
	history_.record(std::make_unique<StanceEdit>(bridge_, slot, before, after, "recall", true));

	if (notifier_ != nullptr) {
		char said[48] = {};
		// Named by where the stance lives on the EXPLORE axis, in zones — the row's own label.
		std::snprintf(said, sizeof(said), "Sound %c at z%+.1f", static_cast<char>('A' + slot),
		              variationHome(slot, index) * 1023.0);
		notifier_->say(said);
	}
	redraw();
}

void SoundStack::storeVariationAt(size_t slot) {
	if (slot >= kMaxSounds) {
		return;
	}
	const StanceEdit::State before { bridge_.soundVariations().sound[slot], liveRideAlong(slot) };
	SoundVariations variations = bridge_.soundVariations();
	// THE SOUND'S EXPLORATION AS IT STANDS — the mock's "clone variation", carrying the whole
	// explore box since V20. Under V19 this is also the PRESERVE verb: the outgoing active entry
	// banks the live values it has been mirroring, the copy becomes the record your next edits
	// mutate, and the banked original stands still — clone-stance FIRST, then edit, is how a
	// stance is kept. Nothing live moves, so this verb writes no parameter at all; what it writes
	// is the memory of the exploration you are at.
	if (!variations.sound[slot].empty()) {
		SoundVariations::List& banked = variations.sound[slot];
		banked.values[banked.active] = before.values;
	}
	if (!storeVariation(variations, slot, before.values)) {
		if (notifier_ != nullptr) {
			notifier_->say("This sound remembers as many stances as it can");
		}
		return;
	}
	const StanceEdit::State after { variations.sound[slot], before.values };

	if (before == after) { return; }

	bridge_.beginSoundVariationsGesture();
	bridge_.setSoundVariations(slot, after.list);
	bridge_.endSoundVariationsGesture();
	history_.record(std::make_unique<StanceEdit>(bridge_, slot, before, after, "stance"));
	selection_.select(slot);

	if (notifier_ != nullptr) {
		char said[64] = {};
		std::snprintf(said, sizeof(said), "Sound %c remembers %d stances", static_cast<char>('A' + slot),
		              static_cast<int>(after.list.count));
		notifier_->say(said);
	}
	redraw();
}

void SoundStack::forgetVariationAt(size_t slot, size_t index) {
	if (slot >= kMaxSounds) {
		return;
	}
	const StanceEdit::State before { bridge_.soundVariations().sound[slot], liveRideAlong(slot) };
	SoundVariations variations = bridge_.soundVariations();
	const bool wasActive = !variations.sound[slot].empty() && index == variations.sound[slot].active;
	if (!forgetVariation(variations, slot, index)) {
		// The last one, or a sound that has stored none at all. Said rather than ignored: a button that
		// does nothing quietly is the thing a hand blames itself for.
		if (notifier_ != nullptr) {
			notifier_->say("A sound is never unplaced — this is its last place");
		}
		return;
	}
	// FORGETTING A PLACE YOU HAD BEEN moves nothing: you forgot a memory, not your feet. FORGETTING
	// THE RECORD YOU ARE STANDING IN cannot leave you standing in it — V19's model has no stepped-off
	// state to remain in — so the neighbour forgetVariation just made active RECALLS: its stored
	// contents come into the parameters, exactly as clicking its row would. (The mock's model, where
	// the live view IS places[act], can only do the same: delete act's record and the view is the new
	// act's.)
	const SoundVariations::List& kept = variations.sound[slot];
	const StanceEdit::State after { kept, wasActive ? kept.values[kept.active] : before.values };

	if (before == after) { return; }

	bridge_.beginSoundVariationsGesture();
	bridge_.setSoundVariations(slot, after.list);
	bridge_.endSoundVariationsGesture();
	if (wasActive) {
		writeRideAlong(bridge_, slot, after.values);
	}
	history_.record(std::make_unique<StanceEdit>(bridge_, slot, before, after, "forget", wasActive));

	if (notifier_ != nullptr) {
		notifier_->say(std::string("Sound ") + static_cast<char>('A' + slot) + " forgot a place");
	}
	redraw();
}

std::optional<size_t> SoundStack::cloneTarget() const {
	// THE FIRST EMPTY SLOT, in slot order. It used to be the first switched-OFF slot, and the difference
	// showed on first use: a patch that pre-seeds all eight slots has no empty
	// ones, so every clone landed on top of a seeded sound and said "cloned over" — which reads as
	// destroying something, because it is. An empty slot is one nothing has been authored in (isEmpty),
	// so a clone now always goes somewhere nothing was, and the confusing case cannot arise.
	//
	// IT IS NOT WHERE THE COPY APPEARS, not since the splice — see `cloneDestination`. What it still
	// answers, and what the button still needs, is whether there is room at all.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (isEmpty(slot)) {
			return slot;
		}
	}
	return std::nullopt;
}

std::vector<size_t> SoundStack::occupiedLane() const {
	std::vector<size_t> lane;
	lane.reserve(kMaxSounds);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (!isEmpty(slot)) {
			lane.push_back(slot);
		}
	}
	return lane;
}

std::optional<size_t> SoundStack::cloneDestination(size_t slot) const {
	const std::optional<size_t> room = spliceRoom(slot);
	if (!room.has_value() || slot >= kMaxSounds || isEmpty(slot)) {
		return std::nullopt;
	}
	// IMMEDIATELY AFTER ITS SOURCE IN THE LIST, which is the slot one further along the lane once the
	// spent one has joined it. Not `slot + 1`: the next slot may be a gap the lane steps over, and the
	// copy belongs beside its original in the list rather than in the next index.
	std::vector<size_t> lane = occupiedLane();
	lane.insert(std::lower_bound(lane.begin(), lane.end(), *room), *room);
	const auto at = std::find(lane.begin(), lane.end(), slot);
	if (at == lane.end() || at + 1 == lane.end()) {
		return std::nullopt;
	}
	// Room found BELOW the source leaves it where it is and the copy takes the next lane slot; room
	// found ABOVE pulls the source itself one place up the lane, so the slot it vacates is where the
	// copy goes. Both are "the lane slot after the source's new position", which is what this is.
	return *room > slot ? *(at + 1) : *at;
}

void SoundStack::clone(size_t slot) {
	if (slot >= kMaxSounds) {
		return;
	}
	const std::optional<size_t> room = spliceRoom(slot);
	if (!room.has_value()) {
		// Every slot is used: there is nowhere to put a copy. Said out loud rather than silently
		// ignored, because a button that does nothing without saying why is the thing a hand blames
		// itself for.
		if (notifier_ != nullptr) {
			// THE ONLY PLACE THE CEILING IS PUT INTO WORDS, now that the new-sound row hides instead of
			// dimming — "clone over" is disabled and a "max 8 sounds" message shown instead. The first
			// half was already true by construction; this is the second.
			// It leads with the LIMIT rather than with the refusal, because the limit is the thing a
			// hand at the ceiling does not know yet.
			notifier_->say("Max 8 sounds — nowhere to clone to");
		}
		return;
	}
	// THE LANE, with the spent slot in it: the sequence of slots the list will occupy once this act is
	// done. Everything below is "which state goes in which of these".
	std::vector<size_t> lane = occupiedLane();
	lane.insert(std::lower_bound(lane.begin(), lane.end(), *room), *room);
	const auto sourceIn = std::find(lane.begin(), lane.end(), slot);
	if (sourceIn == lane.end()) {
		return; // an empty slot has no sound to copy
	}
	const size_t position = static_cast<size_t>(sourceIn - lane.begin());
	// Room found BELOW the source leaves it in place and the copy takes the next lane slot; room found
	// ABOVE pulls the source one place up the lane and the copy takes the slot it vacates.
	const size_t sourceAt = *room > slot ? slot : lane[position - 1];
	const size_t landing = *room > slot ? lane[position + 1] : slot;

	// THE WHOLE IDENTITY, AND THE SWITCH WITH IT, through the one shared pair that Explore's undo uses
	// (sound_identity.h, edit_history.h). A clone that copied less of a sound than an undo restores
	// would be a second opinion about what a sound is; a clone you cannot hear is not a clone.
	//
	// AND THE RIDE-ALONG FAMILY, WHOLE, FROM THE SOURCE — V18: under cloning, the ride-along sound
	// params (now including width, later more) follow the clone and the stance. The copy is the same
	// SOUND, so its character travels: the width it stands at and
	// every stance it remembers, each stance with its own stored values. This OVERTURNS the
	// V14a-era read that took the spent slot's width with the place — width answers
	// how-the-sound-sounds, not where-it-stands — and the family is taken as ONE object so a value
	// member added later is carried by construction instead of being picked up by one line here and
	// missed by the next. INDEPENDENT COPIES, by design: clones are independent copies, not
	// linked — true by construction here because everything moves by value through the param road,
	// and pinned by the independence cases so no future shared-storage shortcut can silently link
	// them.
	//
	// THE PLACE IT STANDS IN IS STILL THE SPENT SLOT'S OWN (A34 holds for place alone now). This
	// read "the target's own", which was the same sentence while the target WAS the empty slot;
	// under a splice the two come apart, and it is the SPENT slot that must be quoted. The sounds
	// that shift carry their placements with them, so the destination index's old place is already
	// in use by whichever sound moved into it — a copy taking that would arrive standing on top of
	// its neighbour. The spent slot's is the place this act actually freed, which keeps the pair
	// separable the moment it exists rather than two stars on one anchor.
	const SlotState source = readSlotState(bridge_, slot);
	const SlotState spent = readSlotState(bridge_, *room);
	// AND A COLOUR OF ITS OWN, which is the one thing here that deliberately does NOT copy. The mock
	// rules it (`forkSound` pushes a fresh hue) and it is the same separability argument that already
	// keeps a clone off its source's anchor: the copy is an independent sound from the moment it
	// exists, and two identical dots would assert a link between them that nothing else in the editor
	// honours. A colour names a sound; a name is the one thing a copy may not inherit — which is also
	// why the hue stands OUTSIDE the RideAlong family the arrival copies whole (V18).
	const uint8_t colour = lowestUnusedHue(bridge_.soundHues(), occupiedLane());
	// AND ITS KNOBS, from the source: per-sound settings follow a cloned sound, as independent copies.
	const SlotState arrival { source.identity, true, source.rideAlong, spent.place, colour, source.knobs, source.curve };

	// The sequence: every sound in list order, with the copy inserted straight after its original.
	std::vector<SlotState> states;
	states.reserve(lane.size());
	for (const size_t at : occupiedLane()) {
		states.push_back(readSlotState(bridge_, at));
		if (at == slot) {
			states.push_back(arrival);
		}
	}

	// AND THE LIKES FOLLOW THEIR SOUNDS (backlog PC: duplicating a sound dropped its likes). A like is
	// keyed by the SLOT it was made on (terrain_likes.h), and the splice moves sounds between slots, so every like is
	// re-keyed to where its sound now stands — without that the copy would inherit whatever like sat in its landing slot
	// and the sounds shifted along would lose theirs to a neighbour. The source's likes, on every engine, are then copied
	// to the landing slot as INDEPENDENT values (the knobs' rule): unliking on one leaves the other alone. Appended in the
	// order they were made, so the copy's newest like is its source's newest. A like on a slot that holds no sound (an
	// orphan a delete left behind) has no sound to follow, and is dropped when the splice spends that slot.
	SlotMoves movedTo {};
	{
		size_t i = 0;
		for (const size_t at : occupiedLane()) {
			movedTo[at] = lane[i++];
			if (at == slot) {
				++i; // the copy's place in the lane
			}
		}
	}
	const TerrainLikes likesBefore = bridge_.terrainLikes();
	TerrainLikes likesAfter = rekeyLikes(likesBefore, movedTo, *room);
	size_t uncopied = 0;
	for (const TerrainLike& like : likesBefore.likes) {
		if (like.slot != slot) {
			continue;
		}
		if (likesAfter.likes.size() >= kMaxTerrainLikes) {
			++uncopied;
			continue;
		}
		TerrainLike copy = like;
		copy.slot = static_cast<uint16_t>(landing);
		likesAfter.likes.push_back(copy);
	}

	std::vector<OrderEdit::Step> steps = laneSteps(bridge_, lane, states);
	if (steps.empty()) {
		return;
	}
	for (const OrderEdit::Step& step : steps) {
		writeSlotState(bridge_, step.slot, step.after);
	}
	std::optional<std::pair<TerrainLikes, TerrainLikes>> likes;
	if (!(likesAfter == likesBefore)) {
		bridge_.setTerrainLikes(likesAfter);
		likes.emplace(likesBefore, likesAfter);
	}
	// ONE UNDO STEP FOR THE WHOLE ACT, every slot the splice touched inside it — and the likes with them. It was one
	// slot's worth while a clone only ever wrote one; a splice that could be walked back a slot at a time would walk
	// through orders the list was never in.
	history_.record(std::make_unique<OrderEdit>(bridge_, std::move(steps), slot, landing, "clone", std::move(likes)));
	// The clone is what you are now working on — the same rule switching a sound on already follows,
	// and for the same reason: you made it in order to change it.
	selection_.select(landing);
	if (notifier_ != nullptr) {
		// ALWAYS "TO" NOW, and the branch that said "over" is gone rather than left unreachable: a clone
		// only ever lands in a slot nothing was authored in, so there is no longer a case where a copy
		// replaces something somebody made. The confusion is answered by the targeting rather than
		// by a better sentence about it.
		// SAID, NOT SILENT, when the patch's likes are full and some of the source's could not come along.
		notifier_->say(std::string("Sound ") + static_cast<char>('A' + sourceAt) + " cloned to "
		               + static_cast<char>('A' + landing)
		               + (uncopied > 0 ? " (likes full: " + std::to_string(uncopied) + " not copied)" : std::string()));
	}
	redraw();
}

namespace {

/// "1st", "2nd", "3rd", "4th" — the tray says WHERE in the list a sound went, because that is what
/// the hand was aiming at. Only ever called with 1..8, so the teens the general rule exists for
/// cannot arise; written as the plain table rather than as the rule, which would be four lines of
/// arithmetic guarding against numbers this list cannot hold.
const char* ordinalSuffix(size_t position) {
	switch (position) {
	case 1:
		return "st";
	case 2:
		return "nd";
	case 3:
		return "rd";
	default:
		return "th";
	}
}

} // namespace

void SoundStack::reorder(size_t from, size_t to) {
	if (from >= kMaxSounds || to >= kMaxSounds || from == to || isEmpty(from) || isEmpty(to)) {
		return;
	}
	const std::vector<size_t> lane = occupiedLane();
	const auto at = std::find(lane.begin(), lane.end(), from);
	const auto onto = std::find(lane.begin(), lane.end(), to);
	if (at == lane.end() || onto == lane.end()) {
		return;
	}

	// THE SOUND TRAVELS WHOLE — identity, switch, stances AND placement. It is an existing sound
	// changing slots rather than a new one being made, so A34 does not apply: leaving the placement
	// behind would teleport every sound the hand rearranged, which is a reorder that changes what the
	// patch sounds like.
	std::vector<SlotState> states;
	states.reserve(lane.size());
	for (const size_t slot : lane) {
		states.push_back(readSlotState(bridge_, slot));
	}
	// MOVE-TO-INDEX, which is a rotate rather than a swap — the mock's own `reorderSound()` splices,
	// and a swap is a different verb that happens to agree with it only for adjacent rows.
	SlotState travelling = states[static_cast<size_t>(at - lane.begin())];
	states.erase(states.begin() + (at - lane.begin()));
	states.insert(states.begin() + (onto - lane.begin()), std::move(travelling));

	// AND THE LIKES WITH THEIR SOUNDS (backlog PC): the same rotate over the slots the sounds came from, so each like is
	// re-keyed to where its sound now stands rather than staying on a slot another sound has moved into.
	std::vector<size_t> came = lane;
	came.erase(came.begin() + (at - lane.begin()));
	came.insert(came.begin() + (onto - lane.begin()), from);
	SlotMoves movedTo {};
	for (size_t i = 0; i < lane.size(); ++i) {
		movedTo[came[i]] = lane[i];
	}
	const TerrainLikes likesBefore = bridge_.terrainLikes();
	const TerrainLikes likesAfter = rekeyLikes(likesBefore, movedTo, std::nullopt);

	std::vector<OrderEdit::Step> steps = laneSteps(bridge_, lane, states);
	if (steps.empty()) {
		return;
	}
	for (const OrderEdit::Step& step : steps) {
		writeSlotState(bridge_, step.slot, step.after);
	}
	std::optional<std::pair<TerrainLikes, TerrainLikes>> likes;
	if (!(likesAfter == likesBefore)) {
		bridge_.setTerrainLikes(likesAfter);
		likes.emplace(likesBefore, likesAfter);
	}
	history_.record(std::make_unique<OrderEdit>(bridge_, std::move(steps), from, to, "reorder", std::move(likes)));
	// THE SELECTION FOLLOWS THE SOUND, not the slot — the same rule delete follows. A card that stayed
	// selected by index would leave the hand editing whichever sound the splice happened to shift into
	// the place it was looking at.
	selection_.select(to);

	if (notifier_ != nullptr) {
		// NAMED BY THE LETTER IT WAS GRABBED BY, and placed by where it went. The letters re-badge —
		// a letter is the name of a SLOT — so after the move that card says something else, and a
		// message using the new letter would be describing a card the hand never touched.
		const size_t position = static_cast<size_t>(onto - lane.begin()) + 1;
		char said[48] = {};
		std::snprintf(said, sizeof(said), "%c moved to %zu%s", static_cast<char>('A' + from), position,
		              ordinalSuffix(position));
		notifier_->say(said);
	}
	redraw();
}

//---------//
// Drawing //
//---------//

void SoundStack::drawCard(visage::Canvas& canvas, size_t slot) const {
	const visage::Bounds card = cardBounds(slot);
	// IN A KIT THE SWITCH IS THE PAD'S MUTE: "on" is a pad that sounds.
	const bool kit = kitMode();
	const bool on = kit ? !padMuted(slot) : enabled(slot);
	const bool selected = selection_.slot() == slot;
	const bool listening = previewing(slot);
	// WHILE A PREVIEW IS LATCHED, EVERY OTHER CARD IS SOMETHING YOU CANNOT HEAR, and the rail says so
	// by falling back — a switched-on sound that is silent because of a listening mode must not look
	// the same as one that is playing. The previewed card keeps full strength even when its own switch
	// is OFF, because preview ignores the switch (preview.h) and this is the one place that fact is
	// visible: the sound you are hearing is drawn as heard.
	const float unheard = anyPreviewing() && !listening ? kUnheardOpacity : 1.0f;
	const float fade = (listening ? 1.0f : (on ? 1.0f : kDisabledOpacity)) * unheard;

	// The card's own ground and hairline. Selected is a raised panel with the mock's own brighter
	// border; unselected is the void with a plain line, so the lit card is the only filled shape in
	// the rail and reads as the one being edited.
	if (selected) {
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(card.x(), card.y(), card.width(), card.height(), kCardRounding);
	}
	// THE CARD NO LONGER DRAWS THE DROP TARGET. It used to take an accent border when a drag would
	// land "on" it, which was misleading: a border on a CARD says the sound would replace
	// that one, and what a reorder actually does is put it in a GAP. The bumper is drawn once, after
	// the cards, by drawDropBumper — so it can also sit above the first and below the last, which a
	// per-card border had no way to express.
	canvas.setColor(selected ? kSelectedBorder : withAlpha(kLine, unheard));
	canvas.roundedRectangleBorder(card.x(), card.y(), card.width(), card.height(), kCardRounding,
	                              kBorder);
	// AND THE CARD BEING LISTENED TO IS RINGED, outside its own border so it reads on top of the
	// selection's. This is the indicator that is always on screen — the rail is drawn beside both
	// surfaces — which is why preview needs no badge in the header: a second place to say one thing is
	// a second place to keep in step with it.
	if (listening) {
		canvas.setColor(kAccent);
		canvas.roundedRectangleBorder(card.x() - 2.0f, card.y() - 2.0f, card.width() + 4.0f,
		                              card.height() + 4.0f, kCardRounding + 2.0f, kBorder);
	}
	// THE HIT PULSE (a kit pad struck): the card's own hue washed over it, as bright as the hit, falling over kPulseMs.
	if (const float lit = kit ? pulseOf(slot) : 0.0f; lit > 0.0f) {
		canvas.setColor(withAlpha(seedHue(slot), 0.35f * lit));
		canvas.roundedRectangle(card.x(), card.y(), card.width(), card.height(), kCardRounding);
		canvas.setColor(withAlpha(seedHue(slot), 0.9f * lit));
		canvas.roundedRectangleBorder(card.x() - 1.0f, card.y() - 1.0f, card.width() + 2.0f, card.height() + 2.0f,
		                              kCardRounding + 1.0f, kBorder);
	}

	const float lineTop = card.y() + kCardPadY;
	const float dotX = card.x() + kCardPadX;
	const float dotY = lineTop + (kCardReadoutSize - kDotSize) * 0.5f;

	// The dot is the star's own hue with the star's own halo — the same object in two places, so the
	// eye can carry a colour from the circle to the list without being told they are the same sound.
	canvas.setBlendMode(visage::BlendMode::Add);
	canvas.setColor(visage::Brush::radial(
	    visage::Gradient(visage::Color(withAlpha(seedHue(slot), 0.55f * fade)),
	                     visage::Color(withAlpha(seedHue(slot), 0.0f))),
	    { dotX + kDotSize * 0.5f, dotY + kDotSize * 0.5f }, kDotGlow, kDotGlow,
	    { dotX + kDotSize * 0.5f, dotY + kDotSize * 0.5f }, 0.0f));
	canvas.circle(dotX + kDotSize * 0.5f - kDotGlow, dotY + kDotSize * 0.5f - kDotGlow,
	              kDotGlow * 2.0f);
	canvas.setBlendMode(visage::BlendMode::Alpha);
	canvas.setColor(withAlpha(seedHue(slot), fade));
	canvas.circle(dotX, dotY, kDotSize);

	const visage::Font& readout = font(kCardReadoutSize);
	const visage::Font& sub = font(kCardSubSize);

	// The letter is the name the whole editor calls this sound by: the star's label, the tour's
	// order, the parameter block's own A..H.
	const std::string letter(1, static_cast<char>('A' + slot));
	const float letterX = dotX + kDotSize + 8.0f;
	canvas.setColor(withAlpha(kInk, fade));
	canvas.text(letter, readout, visage::Font::kLeft, letterX, lineTop, 14.0f, kCardReadoutSize);

	// Zone, as the host prints it: the mock's `z0023.0`, zero-padded so the column does not jump as a
	// scrub crosses a power of ten.
	const double zone = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Zone)));
	char zoneText[24] = {};
	std::snprintf(zoneText, sizeof(zoneText), "z%05.1f", zone);
	// Stopping short of EVERY verb on the line, not just the switch: each button added to `.acts` moves
	// the left edge of that row, and a readout measured against the old edge runs under the new one.
	// Measured against the leftmost, which is preview.
	canvas.text(zoneText, readout, visage::Font::kLeft, letterX + 14.0f, lineTop,
	            previewBounds(slot).x() - 4.0f - (letterX + 14.0f), kCardReadoutSize);

	// The rest of the identity. Offset and gamma are the other two coordinates of the one phase the
	// engine builds at, so they belong beside the zone rather than in a panel of their own.
	const double offset = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Offset)));
	const double gamma = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Gamma)));
	char identity[48] = {};
	std::snprintf(identity, sizeof(identity), "off %.2f   gam %+.2f", offset, gamma);
	float identityWidth = card.width() - 2.0f * kCardPadX;
	if (kit) {
		// A KIT CARD READS AS ITS PAD: its kind and its note, the Level fader beside them.
		const size_t kind = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(slot, DrumControl::Kind))),
		                                 kDrumKindNames.size());
		const long note = std::lround(bridge_.value(static_cast<clap_id>(drumParam(slot, DrumControl::Note))));
		std::snprintf(identity, sizeof(identity), "%s  \xc2\xb7  note %ld", kDrumKindNames[kind], note);
		identityWidth = card.width() * 0.5f - kCardPadX;
	}
	canvas.setColor(withAlpha(kMuted, fade));
	canvas.text(identity, sub, visage::Font::kLeft, dotX, lineTop + kCardReadoutSize + 3.0f, identityWidth, kCardSubSize);

	// THE CLONE BUTTON: two offset rounded outlines, which is what the mock's ⧉ is a picture of. Drawn
	// rather than typed, because A16 established that this editor's font rasterises none of the mock's
	// symbols and a missing glyph is an invisible control.
	//
	// It DIMS WHEN THERE IS NOWHERE TO CLONE TO — all eight sounds switched on — for the same reason
	// undo and redo dim on Explore: a control that looks live and does nothing teaches the hand to
	// distrust the surface. The press still answers, and says why.
	// NOT IN A KIT (hidden, not greyed): a kit's eight pads are always present, so there is nothing to make or remove.
	if (!kit) {
		const visage::Bounds copy = cloneBounds(slot);
		const bool canClone = cloneTarget().has_value();
		canvas.setColor(canClone ? withAlpha(kMuted, fade) : withAlpha(kDisabled, fade));
		const float leaf = kCloneSize * 0.72f;
		const float shift = kCloneSize - leaf;
		canvas.roundedRectangleBorder(copy.x(), copy.y() + shift, leaf, leaf, 2.0f, kBorder);
		canvas.roundedRectangleBorder(copy.x() + shift, copy.y(), leaf, leaf, 2.0f, kBorder);
	}

	// THE PREVIEW BUTTON: an S in a box. Lit in the accent while this sound is the one being heard,
	// muted otherwise — the same two states the switch beside it uses, so the line reads as three
	// controls of one family rather than three separate inventions.
	//
	// AN S IN A BOX AND NOT A PLAY TRIANGLE, decided on the first collapsed-rail render.
	// The verb here is solo — hear this sound by itself — and
	// a play triangle says transport, which this is not: it starts nothing and stops nothing, it
	// chooses which sound the puck is heard through. It also spent one of the three interchangeable
	// filled triangles this editor had grown, which is the collision this avoids.
	//
	// The letter is TYPED and the box is DRAWN, which does not contradict A16: that rule is about
	// symbol glyphs a font may not carry (`➤`, `◂`), and a capital S is in every font that can draw
	// the rest of this card.
	//
	// NEVER DISABLED. Unlike clone, which dims when there is nowhere to put a copy, there is no state
	// in which listening to a sound is impossible: a switched-off sound previews exactly as well as a
	// switched-on one, which is the point of preview ignoring the switch.
	const visage::Bounds listen = previewBounds(slot);
	canvas.setColor(listening ? kAccent : withAlpha(kMuted, fade));
	{
		canvas.roundedRectangleBorder(listen.x(), listen.y(), listen.width(), listen.height(), 2.0f,
		                              kBorder);
		// Optically centred rather than box-centred: a cap-height letter sits low in a box that is
		// centring its whole line box, and the half pixel up is what keeps it off the bottom rule.
		canvas.text("S", font(kSoloLetterSize), visage::Font::kCenter, listen.x(), listen.y() - 0.5f,
		            listen.width(), listen.height());
	}

	// THE DELETE ✕, at the end of the line: the same mark the variation rows use to forget, because it
	// says the same thing at a bigger scale. Never dimmed — a drawn card is by definition a consumed
	// slot, so there is no state in which this verb has nothing to do.
	const visage::Bounds gone = deleteBounds(slot);
	canvas.setColor(withAlpha(kMuted, fade));
	if (!kit) {
		const float inset = 1.5f;
		canvas.segment(gone.x() + inset, gone.y() + inset, gone.right() - inset,
		               gone.bottom() - inset, kBorder, true);
		canvas.segment(gone.x() + inset, gone.bottom() - inset, gone.right() - inset,
		               gone.y() + inset, kBorder, true);
	}

	// The switch: `Enable`, and the other control on the card.
	const visage::Bounds pill = switchBounds(slot);
	canvas.setColor(on ? kSwitchOnBorder : kLine);
	canvas.roundedRectangleBorder(pill.x(), pill.y(), pill.width(), pill.height(),
	                              pill.height() * 0.5f, kBorder);
	const float knob = 6.0f;
	const float knobY = pill.y() + (pill.height() - knob) * 0.5f;
	const float knobX = on ? pill.right() - 1.0f - knob : pill.x() + 1.0f;
	if (on) {
		canvas.setBlendMode(visage::BlendMode::Add);
		canvas.setColor(visage::Brush::radial(
		    visage::Gradient(visage::Color(withAlpha(kAccent, 0.5f)),
		                     visage::Color(withAlpha(kAccent, 0.0f))),
		    { knobX + knob * 0.5f, knobY + knob * 0.5f }, 4.0f, 4.0f,
		    { knobX + knob * 0.5f, knobY + knob * 0.5f }, 0.0f));
		canvas.circle(knobX + knob * 0.5f - 4.0f, knobY + knob * 0.5f - 4.0f, 8.0f);
		canvas.setBlendMode(visage::BlendMode::Alpha);
	}
	canvas.setColor(on ? kAccent : kDisabled);
	canvas.circle(knobX, knobY, knob);

	// THE PLACEMENT ROWS — the mock's `.anc-row`, indented under the card they belong to. They are the
	// sound's anchors, which is the other half of the two-level model: they say where you have to stand
	// to hear the identity above them. One row for a sound that remembers nothing, which is the single
	// row this rail has always drawn; one per remembered stance otherwise.
	for (size_t index = 0; index < variationRows(slot); ++index) {
		const visage::Bounds row = variationBounds(slot, index);
		const bool here = activeStance(slot, index);

		// THE MOCK'S RADIO LIGHT, and what it claims is "this is the record you are editing" — V19:
		// stances are write-through, so there is no stepped-off state for the light to be honest
		// about; the active row moves with the sound, and the light names it. See activeStance.
		const float dotSize = 5.0f;
		const float dotX = row.x() + 14.0f;
		const float dotY = row.y() + (row.height() - dotSize) * 0.5f;
		canvas.setColor(here ? withAlpha(kAccent, fade) : withAlpha(kDisabled, fade));
		if (here) {
			canvas.circle(dotX, dotY, dotSize);
		}
		else {
			canvas.ring(dotX, dotY, dotSize, 1.0f);
		}

		char placement[48] = {};
		// The stance's home on the EXPLORE axis, in zones — a stance is a stored exploration
		// since V20, so its row is named by where in the terrain it recalls, not by a place pair.
		std::snprintf(placement, sizeof(placement), "z%+.1f", variationHome(slot, index) * 1023.0);
		canvas.setColor(withAlpha(here ? kInk : kMuted, fade * 0.9f));
		canvas.text(placement, sub, visage::Font::kLeft, row.x() + 26.0f,
		            row.y() + (row.height() - kCardSubSize) * 0.5f,
		            variationStoreBounds(slot, index).x() - 4.0f - (row.x() + 26.0f), kCardSubSize);

		// The row's two verbs, drawn as geometry for the reason every other glyph here is (A16).
		//
		// STORE IS A PLUS, NOT A SECOND ⧉ — and this is a deliberate departure from the mock, which
		// uses ⧉ for both "clone sound" and "clone variation". On the first build this verb could not be
		// found at all — it was unclear how to select a variant of a sound. The mock tells
		// its two ⧉ apart with a `title` tooltip, and this editor has no hover of any kind to put one
		// in — so the marks themselves have to differ. A plus also says the same thing as the plus on
		// the NEW SOUND row below, which is the honest grouping: both make a thing that was not there.
		const visage::Bounds store = variationStoreBounds(slot, index);
		canvas.setColor(withAlpha(kMuted, fade));
		const float arm = kRowActSize * 0.8f;
		const float cx = store.x() + store.width() * 0.5f;
		const float cy = store.y() + store.height() * 0.5f;
		canvas.segment(cx - arm * 0.5f, cy, cx + arm * 0.5f, cy, kBorder, true);
		canvas.segment(cx, cy - arm * 0.5f, cx, cy + arm * 0.5f, kBorder, true);

		// FORGET DIMS ON THE LAST ROW, because a sound is never unplaced and a control that looks live
		// and refuses is a control that teaches the hand to distrust the surface.
		const visage::Bounds forget = variationForgetBounds(slot, index);
		const bool canForget = variationRows(slot) > 1;
		canvas.setColor(canForget ? withAlpha(kMuted, fade) : withAlpha(kDisabled, fade));
		const float inset = 1.5f;
		canvas.segment(forget.x() + inset, forget.y() + inset, forget.right() - inset,
		               forget.bottom() - inset, kBorder, true);
		canvas.segment(forget.x() + inset, forget.bottom() - inset, forget.right() - inset,
		               forget.y() + inset, kBorder, true);
	}
}

void SoundStack::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();

	canvas.setColor(kVoid);
	canvas.fill(0.0f, 0.0f, w, h);
	// The mock's `.rail.left { border-right: 1px solid var(--line) }` — the seam between the list and
	// the surface it selects for.
	canvas.setColor(kLine);
	canvas.fill(w - 1.0f, 0.0f, 1.0f, h);

	canvas.setColor(kMuted);
	drawTracked(canvas, "SOUNDS", font(kBylineSize), kRailLabelTracking, kPadX, kPadTop - scroll_,
	            kHeadingHeight);

	for (size_t row = 0; row < visibleCount(); ++row) {
		const size_t slot = visibleSlot(row);
		// Skip what is scrolled out of sight. visage clamps a shape to its frame, so this is a saving
		// rather than a correctness measure — but a rail of eight cards redraws on every parameter any
		// of them watches, and half of them are usually off screen.
		if (!slotOnScreen(slot)) {
			continue;
		}
		drawCard(canvas, slot);
	}

	// AND THE BUMPER, ONCE, ON TOP OF THE CARDS — the whole of what a drag has to say. Drawn after
	// them so it is not painted over by the card below it, and drawn here rather than in drawCard
	// because a gap belongs to the LIST and not to either card beside it.
	if (dragging_ && drop_.has_value()) {
		const visage::Bounds bumper = gapBounds(*drop_);
		if (bumper.height() > 0.0f) {
			canvas.setColor(kAccent);
			canvas.roundedRectangle(bumper.x(), bumper.y(), bumper.width(), bumper.height(), 1.0f);
		}
	}

	// THE ROW THAT MAKES A SOUND — the mock's "+ New sound", in the rail where the thing it creates
	// appears.
	//
	// IT HIDES AT THE CEILING RATHER THAN DIMMING, by design: with 8 already present it is hidden
	// completely. The rail then shows exactly what exists and nothing else, and the
	// limit is communicated by the row's ABSENCE rather than by a control sitting there looking
	// broken. Which leaves the clone's refusal as the only place the ceiling is put into words, and
	// its wording carries that now.
	if (newSoundTarget().has_value()) {
		const visage::Bounds row = newRowBounds();
		if (row.bottom() >= 0.0f && row.y() <= h) {
			canvas.setColor(kMuted);
			const float plusSize = 7.0f;
			const float plusX = row.x() + kCardPadX;
			const float plusY = row.y() + (row.height() - plusSize) * 0.5f;
			canvas.segment(plusX, plusY + plusSize * 0.5f, plusX + plusSize, plusY + plusSize * 0.5f,
			               kBorder, true);
			canvas.segment(plusX + plusSize * 0.5f, plusY, plusX + plusSize * 0.5f, plusY + plusSize,
			               kBorder, true);
			canvas.text("NEW SOUND", font(kCardSubSize), visage::Font::kLeft,
			            plusX + plusSize + 8.0f, row.y() + (row.height() - kCardSubSize) * 0.5f,
			            row.width() - kCardPadX - plusSize - 8.0f, kCardSubSize);
		}
	}

	// The scroll indicator, and only when there is something to scroll: a track that is always there
	// is furniture, and a rail this narrow has no room for furniture.
	if (maxScroll() > 0.0f) {
		const float trackX = w - 4.0f;
		const float visible = h / contentHeight();
		const float thumbHeight = std::max(24.0f, h * visible);
		const float travel = h - thumbHeight;
		const float thumbY = travel * (scroll_ / maxScroll());
		canvas.setColor(withAlpha(kLine, 0.9f));
		canvas.roundedRectangle(trackX, thumbY, 2.0f, thumbHeight, 1.0f);
	}
}

} // namespace feathers::ui
