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

#include "plugin/ui/slot_edit.h"
#include "plugin/ui/edit_history.h"

#include "plugin/params.h"
#include "plugin/sound_hues.h"

#include <algorithm>
#include <utility>

namespace feathers::ui {

namespace {

/// What `endGroup` records for a bracket that caught more than one edit: all of them, as one step.
class GroupEdit final : public EditRecord {
public:
	GroupEdit(std::vector<std::unique_ptr<EditRecord>> edits, const char* verb)
	    : edits_(std::move(edits)), verb_(verb) {}

	// LAST-FIRST ON THE WAY BACK, so an edit that read what an earlier one wrote is unwound before it.
	void undo() const override {
		for (auto it = edits_.rbegin(); it != edits_.rend(); ++it) {
			(*it)->undo();
		}
	}
	void redo() const override {
		for (const auto& edit : edits_) {
			edit->redo();
		}
	}
	const char* verb() const override { return verb_; }
	std::optional<size_t> slot() const override { return edits_.front()->slot(); }
	bool live() const override {
		for (const auto& edit : edits_) {
			if (edit->live()) {
				return true;
			}
		}
		return false;
	}

private:
	std::vector<std::unique_ptr<EditRecord>> edits_;
	const char* verb_;
};

/// **A RECORD FROM A NESTED LANE, IN THE OUTER HISTORY** (backlog JB, `EditHistory::forwardTo`): walked only while the
/// lane's host and its panel both live — the records hold them by reference — and dead after.
class ForwardedEdit final : public EditRecord {
public:
	ForwardedEdit(std::weak_ptr<const void> panel, std::weak_ptr<const void> host, std::unique_ptr<EditRecord> inner)
	    : panel_(std::move(panel)), host_(std::move(host)), inner_(std::move(inner)) {}
	void undo() const override {
		if (live()) {
			inner_->undo();
		}
	}
	void redo() const override {
		if (live()) {
			inner_->redo();
		}
	}
	const char* verb() const override { return inner_->verb(); }
	std::optional<size_t> slot() const override { return live() ? inner_->slot() : std::nullopt; }
	bool live() const override { return !panel_.expired() && !host_.expired() && inner_->live(); }

private:
	std::weak_ptr<const void> panel_;
	std::weak_ptr<const void> host_;
	std::unique_ptr<EditRecord> inner_;
};

bool slotEnabled(const ParamBridge& bridge, size_t slot) {
	const auto id = static_cast<clap_id>(soundParam(slot, SoundField::Enable));
	return steppedIndex(bridge.value(id), kEnableNames.size()) != 0;
}

} // namespace

SlotState readSlotState(const ParamBridge& bridge, size_t slot) {
	const size_t at = std::min(slot, kMaxSounds - 1);
	SlotState state { readIdentity(bridge, at), slotEnabled(bridge, at),
		               // The ride-along family: the live width off the parameters (its siblings travel as
		               // the identity above — see the struct), the stance list off the bridge.
		               { bridge.value(static_cast<clap_id>(soundParam(at, SoundField::Width))),
		                 bridge.soundVariations().sound[at] },
		               { bridge.value(static_cast<clap_id>(soundParam(at, SoundField::PlaceX))),
		                 bridge.value(static_cast<clap_id>(soundParam(at, SoundField::PlaceY))) },
		               bridge.soundHues().index[at] };
	for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
		state.knobs[k] = bridge.value(static_cast<clap_id>(perSoundKnobParam(at, k)));
	}
	state.curve = bridge.spectralCurve(at);
	return state;
}

std::array<double, kPerSoundKnobCount> untouchedKnobs(size_t slot) {
	std::array<double, kPerSoundKnobCount> knobs {};
	for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(perSoundKnobParam(std::min(slot, kMaxSounds - 1), k)));
		knobs[k] = desc != nullptr ? desc->def : 0.0;
	}
	return knobs;
}

void writeSlotState(ParamBridge& bridge, size_t slot, const SlotState& state) {
	const size_t at = std::min(slot, kMaxSounds - 1);
	writeIdentity(bridge, at, state.identity);
	// THE SWITCH LAST, and only when it moved. Last because a slot coming back on should already sound
	// like what it was rather than like whatever was left in it; and only when it moved because an
	// unconditional write would post a gesture for a value nobody changed, which is an undo step in the
	// host for an edit that did not happen.
	// Where it stands, when that moved. Guarded like the rest: a scrub that never touched the
	// placement must not post a gesture for it.
	const auto place = [&](SoundField field, double value) {
		const auto id = static_cast<clap_id>(soundParam(at, field));
		if (bridge.value(id) == value) {
			return;
		}
		bridge.beginGesture(id);
		bridge.setValue(id, value);
		bridge.endGesture(id);
	};
	place(SoundField::PlaceX, state.place.x);
	place(SoundField::PlaceY, state.place.y);
	// The family's live width, through the same guarded write; its sibling members restore with
	// the identity above.
	place(SoundField::Width, state.rideAlong.width);
	// AND THE SOUND'S KNOBS, each through the same guarded write: an undo of a scrub posts nothing for a knob it never moved.
	for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
		const auto id = static_cast<clap_id>(perSoundKnobParam(at, k));
		if (bridge.value(id) != state.knobs[k]) {
			bridge.beginGesture(id);
			bridge.setValue(id, state.knobs[k]);
			bridge.endGesture(id);
		}
	}
	// AND ITS CURVE, when it moved (setSpectralCurve is not an edit for a curve that did not).
	bridge.setSpectralCurve(at, state.curve);
	// And the family's list-shaped member, when it differs. Guarded like every other channel here:
	// an unconditional write would open a bracket, mark the project dirty and tell the host a patch
	// changed because somebody undid a scrub that never touched a stance.
	if (!(bridge.soundVariations().sound[at] == state.rideAlong.stances)) {
		bridge.beginSoundVariationsGesture();
		bridge.setSoundVariations(at, state.rideAlong.stances);
		bridge.endSoundVariationsGesture();
	}
	// The colour, when it moved. Guarded like every other channel here: an unconditional write would
	// mark the project dirty because somebody undid a scrub that never touched a colour.
	if (bridge.soundHues().index[at] != state.hue) {
		bridge.beginSoundHuesGesture();
		bridge.setSoundHue(at, state.hue);
		bridge.endSoundHuesGesture();
	}
	if (slotEnabled(bridge, at) != state.enabled) {
		const auto id = static_cast<clap_id>(soundParam(at, SoundField::Enable));
		bridge.beginGesture(id);
		bridge.setValue(id, state.enabled ? 1.0 : 0.0);
		bridge.endGesture(id);
	}
}

void EditHistory::forwardTo(EditHistory* outer, std::weak_ptr<const void> hostLife) {
	forward_ = outer != this ? outer : nullptr;
	forwardLife_ = forward_ != nullptr ? std::weak_ptr<const void>(forward_->life_) : std::weak_ptr<const void> {};
	hostLife_ = std::move(hostLife);
}

void EditHistory::record(std::unique_ptr<EditRecord> edit) {
	if (edit == nullptr) {
		return;
	}
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		outer->record(std::make_unique<ForwardedEdit>(std::weak_ptr<const void>(life_), hostLife_, std::move(edit)));
		return;
	}
	if (groupDepth_ > 0) {
		pending_.push_back(std::move(edit));
		return;
	}
	entries_.resize(at_);
	serials_.resize(at_);
	entries_.push_back(std::move(edit));
	serials_.push_back(nextSerial_++);
	at_ = entries_.size();
	tell();
	// The follow-on-record road (see EditRecord::followsOnRecord): the stance switch is an edit the
	// chart should turn toward as it happens, not only when it is walked back.
	if (observer_ != nullptr && entries_.size() > 0 && at_ > 0
	    && entries_[at_ - 1]->followsOnRecord()) {
		observer_->editRecorded(*entries_[at_ - 1]);
	}
}

void EditHistory::beginGroup(const char* verb, uint64_t amends) {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		outer->beginGroup(verb, amends);
		return;
	}
	if (groupDepth_++ == 0) {
		groupVerb_ = verb != nullptr ? verb : "";
		groupAmends_ = amends;
		// **THE CORNERS GO DARK AS THE ACT OPENS** (backlog JB, from IV): `canUndo`/`canRedo` refuse while a
		// group is open, so a button that drew itself lit a moment ago is now lying until something repaints it.
		tell();
	}
}

void EditHistory::endGroup() {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		outer->endGroup();
		return;
	}
	if (groupDepth_ == 0 || --groupDepth_ > 0) {
		return;
	}
	std::vector<std::unique_ptr<EditRecord>> caught = std::move(pending_);
	pending_.clear();
	const uint64_t amends = std::exchange(groupAmends_, 0);
	if (caught.empty()) {
		// AND LIGHT AGAIN AS IT CLOSES, even when it caught nothing — a record or an amend tells on its own.
		tell();
		return;
	}
	std::unique_ptr<EditRecord> act = caught.size() == 1
	                                      ? std::move(caught.front())
	                                      : std::make_unique<GroupEdit>(std::move(caught), groupVerb_);
	// A CONTINUATION WHOSE TIP HAS MOVED ON IS ITS OWN STEP — never folded into somebody else's.
	if (amends != 0 && amendLast(amends, std::move(act))) {
		return;
	}
	record(std::move(act));
}

uint64_t EditHistory::lastSerial() const noexcept {
	if (const EditHistory* outer = forwardTarget(); outer != nullptr) {
		return outer->lastSerial();
	}
	// THE TIP IS THE LAST ENTRY AND ONLY WHILE NOTHING IS UNDONE: an entry with a redo tail above it is
	// not "the latest step", and folding into it would silently discard the tail's meaning.
	if (at_ == 0 || at_ != entries_.size()) {
		return 0;
	}
	return serials_[at_ - 1];
}

bool EditHistory::amendLast(uint64_t serial, std::unique_ptr<EditRecord>&& edit) {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		// REFUSED BEFORE THE WRAP, so a refusal leaves `edit` with the caller as the contract says.
		if (edit == nullptr || serial == 0 || outer->groupOpen() || outer->lastSerial() != serial) {
			return false;
		}
		auto wrapped = std::make_unique<ForwardedEdit>(std::weak_ptr<const void>(life_), hostLife_, std::move(edit));
		return outer->amendLast(serial, std::move(wrapped));
	}
	if (edit == nullptr || serial == 0 || groupDepth_ > 0 || lastSerial() != serial) {
		return false;
	}
	const char* verb = edit->verb();
	std::vector<std::unique_ptr<EditRecord>> both;
	both.push_back(std::move(entries_[at_ - 1]));
	both.push_back(std::move(edit));
	entries_[at_ - 1] = std::make_unique<GroupEdit>(std::move(both), verb);
	serials_[at_ - 1] = nextSerial_++;
	tell();
	return true;
}

bool EditHistory::retractLast(uint64_t serial) {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		return outer->retractLast(serial);
	}
	if (serial == 0 || groupDepth_ > 0 || lastSerial() != serial) {
		return false;
	}
	// UNDONE, THEN FORGOTTEN — and nobody is told "Undo": the hand did not ask for one, it abandoned an act
	// whose first half had already been written down.
	entries_[at_ - 1]->undo();
	entries_.pop_back();
	serials_.pop_back();
	at_ = entries_.size();
	tell();
	return true;
}

void recordSlot(EditHistory& history, ParamBridge& bridge, size_t slot, const SlotState& before,
                             const SlotState& after, const char* verb) {
	if (before == after) {
		return;
	}
	history.record(std::make_unique<SlotEdit>(bridge, slot, before, after, verb));
}

bool EditHistory::canUndo() const noexcept {
	if (const EditHistory* outer = forwardTarget(); outer != nullptr) {
		return outer->canUndo();
	}
	if (groupDepth_ > 0) {
		return false;
	}
	for (size_t i = at_; i > 0; --i) {
		if (entries_[i - 1]->live()) {
			return true;
		}
	}
	return false;
}

bool EditHistory::canRedo() const noexcept {
	if (const EditHistory* outer = forwardTarget(); outer != nullptr) {
		return outer->canRedo();
	}
	if (groupDepth_ > 0) {
		return false;
	}
	for (size_t i = at_; i < entries_.size(); ++i) {
		if (entries_[i]->live()) {
			return true;
		}
	}
	return false;
}

void EditHistory::undo() {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		outer->undo();
		return;
	}
	if (!canUndo()) {
		return;
	}
	// A DEAD RECORD (a nested lane gone, backlog JB) IS STEPPED PAST: the keystroke goes to the next live one.
	while (at_ > 0 && !entries_[at_ - 1]->live()) {
		--at_;
	}
	--at_;
	const EditRecord& edit = *entries_[at_];
	edit.undo();
	if (observer_ != nullptr) {
		observer_->editApplied(edit, "Undo");
	}
	tell();
}

void EditHistory::redo() {
	if (EditHistory* outer = forwardTarget(); outer != nullptr) {
		outer->redo();
		return;
	}
	if (!canRedo()) {
		return;
	}
	while (at_ < entries_.size() && !entries_[at_]->live()) {
		++at_;
	}
	const EditRecord& edit = *entries_[at_];
	++at_;
	edit.redo();
	if (observer_ != nullptr) {
		observer_->editApplied(edit, "Redo");
	}
	tell();
}

void EditHistory::clear() {
	observer_ = nullptr;
	entries_.clear();
	at_ = 0;
	serials_.clear();
	pending_.clear();
	groupDepth_ = 0;
	groupAmends_ = 0;
	watchers_.clear();
}

} // namespace feathers::ui
