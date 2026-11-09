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

#include "plugin/ui/frame_watchers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// THE EDITOR'S ONE UNDO STACK, and nothing about what an edit IS.
//
// SPLIT SO A DEVICE'S CARD CAN RECORD AN EDIT WITHOUT THE SYNTH. This file also held `RideAlong`,
// `SlotState` and `SlotEdit` — Feathers' own sound-slot records — which put `plugin/gui_bridge.h`
// and `plugin/ui/sound_identity.h`, and the parameter table behind them, into every widget that
// only wanted an undo step. Those are in `slot_edit.h` now. Measured with a link probe: this file
// was the LAST chokepoint keeping `knob.cpp`, `controls.cpp` and `generic_card.cpp` coupled to
// `plugin/params.h`.
//
// THE STACK IS SHARED AND THE RECORDS ARE NOT, which is the line the split follows: this file says a
// record can be undone and redone and says nothing about what it edits. That is the argument the file
// already made about itself — it "was written so a verb it has never heard of can be recorded by
// whoever wrote the verb" — carried one step further, to a verb belonging to a device this build may
// never have heard of either.

namespace feathers::ui {

/// One undoable act, and how to walk it in either direction.
///
/// Abstract so that a verb this file has never heard of can be recorded by the author who wrote it.
/// The mod panel's routings, a reorder, a delete: each writes one of these over its own values and
/// pushes it, and nothing here needs to know what it did.
class EditRecord {
public:
	virtual ~EditRecord() = default;

	/// Put the world back the way it was, and then forward again. Called on the editor's thread only.
	virtual void undo() const = 0;
	virtual void redo() const = 0;

	/// What to say in the tray. Present tense of the verb that made it — "Clone", "Scrub" — because
	/// the tray prefixes it with which direction it is walking.
	virtual const char* verb() const = 0;

	/// Which slot this was about, when it was about one. Used by a surface to point itself at what
	/// just moved; an edit that is not about a single sound answers nothing and nothing is pointed.
	virtual std::optional<size_t> slot() const { return std::nullopt; }

	/// Whether RECORDING this edit should point the observing surface at its sound, the way walking
	/// it in the history already does. FALSE by default and deliberately narrow: Explore's own
	/// gestures (scrub, mutate, the corners) manage their view mid-act — a recentre at record time
	/// would move the ground the gesture was measured against — and the rail's structural verbs
	/// already move the selection, which the chart follows. The stance SWITCH is the case this
	/// exists for (the terrain must re-centre when the variant switches): a same-slot
	/// switch changes no selection and no identity, so without this no road asks the view to
	/// re-settle on the sound.
	virtual bool followsOnRecord() const { return false; }

	/// **WHETHER WALKING IT CAN STILL DO ANYTHING** (backlog JB). A record forwarded from a nested lane's history into
	/// the outermost one holds that lane's host and panel by reference; once either is gone it answers false, does
	/// nothing if walked, and the history steps past it rather than spend a keystroke on it.
	[[nodiscard]] virtual bool live() const { return true; }
};

class EditObserver {
public:
	virtual ~EditObserver() = default;
	/// `verb` is what to say — already the whole line, "Undo" or "Redo".
	virtual void editApplied(const EditRecord& record, const char* verb) = 0;
	/// A record that follows-on-record just landed LIVE (see EditRecord::followsOnRecord). The same
	/// pointing as editApplied through the same road, minus the tray line — the verb announced
	/// itself when it ran.
	virtual void editRecorded(const EditRecord& record) { (void)record; }
};

class EditHistory;

/// **A CONTROL THAT RECORDS ITS OWN UNDO STEPS, AND WHERE.** Every value control that writes a step at
/// `endGesture` holds the history it writes into; this lets a widget ask for it through the control, as
/// `RoutedControl` lets it ask for the panel state, rather than every construction site handing it over.
/// The typed-value field needs it (backlog LS): a triple-click's reset has already recorded a step by the
/// time the field opens, and what the field decides must fold into that step or take it back.
/// A control that does not implement it records nothing a widget could fold into.
class RecordsEdits {
public:
	virtual ~RecordsEdits() = default;
	[[nodiscard]] virtual EditHistory* recordsInto() const = 0;
};

class EditHistory {
public:
	/// Remember an act. Truncates the redo tail, which is what every editor does and what stops a
	/// branch you have left from being silently re-entered.
	void record(std::unique_ptr<EditRecord> edit);

	/// **ONE HISTORY PER PLUGIN: A NESTED LANE'S RECORDS GO TO THE OUTERMOST ONE** (backlog JB). Set on a nested panel's
	/// history (a Chain's or a site's window, a SKLITTER rail) with the history of the host it sits in, and every verb of
	/// this one — record, groups, amend, the walk, the watch — is that one's; nested again, the hop repeats out to the
	/// plugin's own. Each record forwarded is wrapped so it goes dead (`EditRecord::live`) once `hostLife` (the nested
	/// lane's bridge, `RackBridge::fragmentLife`) or THIS history (its panel, whose state the records hold) is gone: the
	/// weak-token rule of O6. A record made while the outer history is gone, or with none set, stays here. Null: local.
	void forwardTo(EditHistory* outer, std::weak_ptr<const void> hostLife);
	[[nodiscard]] EditHistory* forwardsTo() const noexcept { return forwardTarget(); }

	/// **ONE ACT ACROSS SEVERAL CONTROLS, RECORDED AS ONE STEP.** Every `record` between the two lands
	/// in one entry, walked back last-first. Nesting `DeviceControl` gestures nests nothing — each
	/// `endGesture` records its own step — so a surface that writes two controls as one move (the
	/// perform pad's point is `Perf X` AND `Perf Y`) cost two undos, and could be taken back half way.
	/// Brackets nest; only the outermost `endGroup` records. A group that caught nothing records
	/// nothing, and a group of one records that one as itself.
	///
	/// `amends`, when it is not 0, names the entry this act CONTINUES: if that entry is still the tip
	/// when the group closes, what the group caught is folded into it (`amendLast`) rather than added
	/// after it. Otherwise the group records its own step as usual.
	void beginGroup(const char* verb, uint64_t amends = 0);
	void endGroup();
	[[nodiscard]] bool groupOpen() const noexcept {
		return forwardTarget() != nullptr ? forwardTarget()->groupOpen() : groupDepth_ > 0;
	}

	/// **THE TIP'S IDENTITY — a serial, not an address.** Every entry is stamped from a counter that
	/// only rises, so a surface can remember "the step I just made" and later ask whether it is still
	/// the latest thing on the stack with nothing recorded after it and nothing undone. An address
	/// would not do: a freed record's address can be handed to the next one (CLAUDE.md's ABA rule).
	/// 0 when there is no tip — an empty history, or one walked back so the tip is in the redo tail.
	[[nodiscard]] uint64_t lastSerial() const noexcept;

	/// **FOLD `edit` INTO THE TIP**, so one undo takes back both it and what the tip already held.
	/// Refuses — returns false, records nothing and leaves `edit` with the caller — unless `serial` is
	/// still the tip's (see `lastSerial`). The combined entry keeps the new edit's verb and gets a fresh
	/// serial.
	bool amendLast(uint64_t serial, std::unique_ptr<EditRecord>&& edit);

	/// **TAKE THE TIP BACK AS IF IT HAD NEVER BEEN MADE** — undone, and gone, with no redo tail left
	/// behind it and nobody told "Undo". Refuses (false, nothing touched) unless `serial` is still the
	/// tip's and no group is open. For a gesture that recorded a step on the way to a decision the hand
	/// then abandons: a triple-click's reset, when the field it opened is cancelled (backlog LS).
	bool retractLast(uint64_t serial);

	/// **NOT WHILE A GROUP IS OPEN.** A group is one act still in progress — the perform pad's is open
	/// from press to release — and walking the stack under it would leave the act's controls holding a
	/// "before" from the far side of the undo, so its step would put back a state that no longer existed.
	/// Refusing rather than closing the group early: closing would let the act's remaining records land
	/// OUTSIDE it, which is the two-steps-for-one defect the group exists to fix.
	bool canUndo() const noexcept;
	bool canRedo() const noexcept;
	void undo();
	void redo();

	size_t length() const noexcept { return forwardTarget() != nullptr ? forwardTarget()->length() : entries_.size(); }

	/// Who gets told after a move. One at a time and nullable: the editor registers its stage and
	/// clears it in teardown, and a history with nobody listening still walks correctly — it just
	/// moves values without moving anybody's view.
	void observe(EditObserver* observer) noexcept { observer_ = observer; }

	/// Forget the observer and every entry. Called from the editor's teardown, because the records
	/// hold a reference to the bridge and the observer is a widget.
	void clear();

	/// Redraw `frame` whenever the history moves — the undo and redo corners light by it.
	[[nodiscard]] Watch watch(visage::Frame* frame) {
		return forwardTarget() != nullptr ? forwardTarget()->watch(frame) : watchers_.watch(frame);
	}

	/// HOW MANY FRAMES ARE ON THIS LIST. For the case that requires every watcher list in the editor
	/// to come back to its baseline across a card cycle.
	///
	/// IT EXISTS BECAUSE THIS LIST HAS NO EXIT. `forgetFrameTree` reaches the registry, the state
	/// channel and the modulation channel; it cannot reach this one, and every subscriber here is an
	/// editor-lifetime surface — which is a fact about who happens to have registered, not a
	/// guarantee. The three shipped instances of the dangling-watcher bug all had that same property
	/// right up until a card became removable. A count that must return to where it started is what
	/// notices the day a dying frame joins.
	size_t watcherCountForTest() const noexcept { return watchers_.count(); }

	void clearWatchers() { watchers_.clear(); }

	/// HOW MANY FRAMES THIS HISTORY HAS TOLD TO REDRAW, summed over its life — the count `FrameWatchers::notify`
	/// returns, so it cannot rise without the notification. A repaint is unobservable headlessly (CLAUDE.md);
	/// this is what lets a case see that opening and closing a group told the corners.
	size_t framesToldForTest() const noexcept { return framesTold_; }

private:
	/// Every notification goes through here, so `framesToldForTest` is the notifications' own count.
	void tell() { framesTold_ += watchers_.notify(); }
	size_t framesTold_ = 0;

	std::vector<std::unique_ptr<EditRecord>> entries_;
	size_t at_ = 0;
	/// What an open group has caught so far, and how deep the bracketing is.
	/// One serial per entry, beside it. Never 0, never reused.
	std::vector<uint64_t> serials_;
	uint64_t nextSerial_ = 1;
	std::vector<std::unique_ptr<EditRecord>> pending_;
	size_t groupDepth_ = 0;
	const char* groupVerb_ = "";
	uint64_t groupAmends_ = 0;
	EditObserver* observer_ = nullptr;
	/// FORWARDING (`forwardTo`): the outer history while its own life token stands, and what each record is tied to.
	[[nodiscard]] EditHistory* forwardTarget() const noexcept {
		return forward_ != nullptr && !forwardLife_.expired() ? forward_ : nullptr;
	}
	EditHistory* forward_ = nullptr;
	std::weak_ptr<const void> forwardLife_;
	std::weak_ptr<const void> hostLife_;
	/// THIS HISTORY'S OWN LIFE, which a record forwarded from it holds weakly, and an inner history forwarding here too.
	std::shared_ptr<const int> life_ = std::make_shared<const int>(0);
	FrameWatchers watchers_;
};

} // namespace feathers::ui
