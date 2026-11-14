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

#include "plugin/ui/edit_history.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/slot_copy_menu.h"
#include "plugin/ui/pattern_card.h"

#include "mod/arp/pattern_device.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <span>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>

namespace feathers::ui {

namespace {
namespace pt = feathers::mod::arp::pat;
namespace pn = feathers::mod::arp::pattern;
using pt::Field;
using pt::Global;
} // namespace

//------------------------------------------------------------------------------------------------//
// Rotate's fold                                                                                  //
//------------------------------------------------------------------------------------------------//

namespace {
/// THE ROTATE EACH PATTERN WAS LAST SEEN AT, per device — the one shared watch (see the header).
std::map<DeviceInstanceId, std::array<pt::PatternSettings, pt::kPatterns>>& rotateWatch() {
	static std::map<DeviceInstanceId, std::array<pt::PatternSettings, pt::kPatterns>> seen;
	return seen;
}
pt::Settings settingsNow(const backplane::DeviceHost& host, DeviceInstanceId id) {
	std::vector<float> row(pt::kParamCount);
	const size_t present = host.fieldsPresentAt(id);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = i < present ? host.fieldAt(id, i) : static_cast<float>(pt::kParamDescs[i].def);
	}
	return pt::settingsOf(row.data(), row.size());
}
} // namespace

void resyncRotateWatch(backplane::DeviceHost& host, DeviceInstanceId id) {
	rotateWatch()[id] = settingsNow(host, id).patterns;
}

bool foldRotateIfTurned(backplane::DeviceHost& host, DeviceInstanceId id) {
	auto& seen = rotateWatch();
	const pt::Settings s = settingsNow(host, id);
	const auto known = seen.find(id);
	if (known == seen.end()) {
		seen[id] = s.patterns;
		return false;
	}
	std::array<pt::PatternSettings, pt::kPatterns>& last = known->second;
	bool folded = false;
	std::vector<float> block(pt::kHandWords);
	pt::HandState state {};
	bool loaded = false;
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		if (s.patterns[p].rotate != last[p].rotate) {
			if (!loaded) {
				state = host.blockAt(id, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
				loaded = true;
			}
			// THE TURN IS BANKED AT THE PLACE THE PATTERN STOOD WHEN IT TURNED: the settings as last seen, at the old rotate.
			state[p] = pt::foldRotate(state[p], last[p], s.patterns[p].rotate);
			folded = true;
		}
	}
	last = s.patterns;
	if (folded) {
		pt::writeHandState(state, block.data());
		(void)host.setBlock(id, 1, block);
	}
	return folded;
}

//------------------------------------------------------------------------------------------------//
// The tab menu                                                                                   //
//------------------------------------------------------------------------------------------------//

namespace {

/// ONE TAB-MENU VERB AS ONE UNDO STEP: the fields it moved, and the hand's and the edits' blocks before and after.
class PatternSlotsEdit final : public EditRecord {
public:
	PatternSlotsEdit(backplane::DeviceHost& host, DeviceInstanceId id, std::vector<DeviceFieldChange> fields,
	                 std::vector<float> handBefore, std::vector<float> handAfter, std::vector<float> editsBefore,
	                 std::vector<float> editsAfter, const char* verb)
	    : host_(&host), id_(id), fields_(std::move(fields)), handBefore_(std::move(handBefore)), handAfter_(std::move(handAfter)),
	      editsBefore_(std::move(editsBefore)), editsAfter_(std::move(editsAfter)), verb_(verb) {}
	void undo() const override { apply(true); }
	void redo() const override { apply(false); }
	const char* verb() const override { return verb_; }

private:
	void apply(bool back) const {
		{
			backplane::ControlGestureScope gesture(*host_, id_);
			for (const DeviceFieldChange& c : fields_) {
				gesture.setField(c.control, back ? c.before : c.after);
			}
		}
		(void)host_->setBlock(id_, 1, back ? handBefore_ : handAfter_);
		(void)host_->setBlock(id_, pt::kEditsBlock, back ? editsBefore_ : editsAfter_);
		resyncRotateWatch(*host_, id_); // the residue came back with its Rotate: nothing to fold
	}
	backplane::DeviceHost* host_;
	DeviceInstanceId id_;
	std::vector<DeviceFieldChange> fields_;
	std::vector<float> handBefore_, handAfter_, editsBefore_, editsAfter_;
	const char* verb_;
};

std::vector<float> blockOr(const backplane::DeviceHost& host, DeviceInstanceId id, size_t index, size_t words,
                           const std::function<void(float*)>& fallback) {
	std::vector<float> block(words);
	if (!host.blockAt(id, index, block)) {
		fallback(block.data());
	}
	return block;
}

} // namespace

std::vector<std::pair<int, std::string>> patternTabMenuOptions(size_t self) {
	std::vector<std::pair<int, std::string>> options;
	options.emplace_back(kPatternMenuReset, "Reset to default");
	appendCopyToOptions(options, pt::kPatterns, self, [](size_t p) { return std::to_string(p + 1); }, kPatternMenuCopyAll);
	return options;
}

void choosePatternTabMenu(backplane::DeviceHost& host, DeviceInstanceId id, size_t from, int choice, EditHistory* history) {
	if (from >= pt::kPatterns) {
		return;
	}
	std::vector<size_t> targets;
	const bool reset = choice == kPatternMenuReset;
	if (reset) {
		targets.push_back(from);
	}
	else if (choice == kPatternMenuCopyAll) {
		for (size_t p = 0; p < pt::kPatterns; ++p) {
			if (p != from) {
				targets.push_back(p);
			}
		}
	}
	else if (choice >= 0 && static_cast<size_t>(choice) < pt::kPatterns && static_cast<size_t>(choice) != from) {
		targets.push_back(static_cast<size_t>(choice));
	}
	if (targets.empty()) {
		return;
	}
	const std::vector<float> handBefore = blockOr(host, id, 1, pt::kHandWords,
	                                              [](float* out) { pt::writeHandState(pt::HandState {}, out); });
	const std::vector<float> editsBefore = blockOr(host, id, pt::kEditsBlock, pt::kEditsWords,
	                                               [](float* out) { pt::writeEditState(pt::EditState {}, out); });
	pt::HandState hand = pt::handStateOf(handBefore.data(), handBefore.size());
	pt::EditState edits = pt::editStateOf(editsBefore.data(), editsBefore.size());
	std::vector<DeviceFieldChange> fields;
	{
		backplane::ControlGestureScope gesture(host, id);
		for (const size_t t : targets) {
			for (size_t k = 0; k < pt::kFieldCount; ++k) {
				const auto f = static_cast<pt::Field>(k);
				if (f == pt::Field::Enable || f == pt::Field::Solo) {
					continue; // THE SELECTOR'S RANGE IS THE USER'S, not the pattern's
				}
				const size_t to = pt::at(t, f);
				const float want = reset ? static_cast<float>(pt::kParamDescs[to].def) : host.fieldAt(id, pt::at(from, f));
				const float before = host.fieldAt(id, to);
				if (before == want) {
					continue;
				}
				gesture.setField(to, want);
				const float after = host.fieldAt(id, to);
				if (after != before) {
					fields.push_back(DeviceFieldChange { to, before, after });
				}
			}
			hand[t] = reset ? pt::HandSlot {} : hand[from];
			edits[t] = reset ? pt::EditOverlay {} : edits[from];
		}
	}
	std::vector<float> handAfter(pt::kHandWords);
	pt::writeHandState(hand, handAfter.data());
	std::vector<float> editsAfter(pt::kEditsWords);
	pt::writeEditState(edits, editsAfter.data());
	(void)host.setBlock(id, 1, handAfter);
	(void)host.setBlock(id, pt::kEditsBlock, editsAfter);
	// THE TARGETS' ROTATE CAME WITH THEIR RESIDUE: the watch takes the row as it now stands, so nothing is folded twice.
	resyncRotateWatch(host, id);
	if (history != nullptr && (!fields.empty() || handAfter != handBefore || editsAfter != editsBefore)) {
		const char* verb = reset ? "Reset pattern" : "Copy pattern";
		history->record(std::make_unique<PatternSlotsEdit>(host, id, std::move(fields), handBefore, handAfter, editsBefore,
		                                                   editsAfter, verb));
	}
}

bool pressPatternSolo(backplane::DeviceHost& host, DeviceInstanceId id, size_t tab, bool additive, EditHistory* history) {
	if (tab >= pt::kPatterns) {
		return false;
	}
	std::array<float, pt::kPatterns> before {};
	size_t soloed = 0;
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		before[p] = host.fieldAt(id, pt::at(p, Field::Solo));
		soloed += before[p] >= 0.5f ? 1u : 0u;
	}
	const bool mine = before[tab] >= 0.5f;
	std::array<float, pt::kPatterns> want {};
	if (additive) {
		want = before; // CTRL/CMD: this tab in or out of the soloed set, the others as they are
		want[tab] = mine ? 0.0f : 1.0f;
	}
	else if (!(mine && soloed == 1)) {
		want[tab] = 1.0f; // A RADIO: this tab alone (the only solo pressed again clears it: `want` stays all off)
	}
	std::vector<DeviceFieldChange> changes;
	{
		backplane::ControlGestureScope gesture(host, id);
		for (size_t p = 0; p < pt::kPatterns; ++p) {
			const size_t control = pt::at(p, Field::Solo);
			if ((want[p] >= 0.5f) == (before[p] >= 0.5f)) {
				continue;
			}
			gesture.setField(control, want[p]);
			changes.push_back(DeviceFieldChange { control, before[p], host.fieldAt(id, control) });
		}
	}
	if (history != nullptr && !changes.empty()) {
		history->record(std::make_unique<DeviceFieldsEdit>(host, id, std::move(changes), "Solo"));
	}
	return want[tab] >= 0.5f;
}

void showPatternTabMenu(backplane::DeviceHost& host, DeviceInstanceId id, size_t from, EditHistory* history,
                        visage::Frame& source, visage::Point at, std::function<void()> exportMidi) {
	visage::PopupMenu menu;
	for (const auto& option : patternTabMenuOptions(from)) {
		menu.addOption(option.first, option.second);
	}
	if (exportMidi) {
		menu.addOption(kPatternMenuExportMidi, "Export MIDI…");
	}
	backplane::DeviceHost* hostPtr = &host;
	menu.onSelection() = [hostPtr, id, from, history, exportMidi](int choice) {
		if (choice == kPatternMenuExportMidi) {
			if (exportMidi) {
				exportMidi();
			}
			return;
		}
		choosePatternTabMenu(*hostPtr, id, from, choice, history);
	};
	showMenuAt(std::move(menu), source, { at.x, at.y, 0.0f, 0.0f });
}

//------------------------------------------------------------------------------------------------//
// The lane                                                                                       //
//------------------------------------------------------------------------------------------------//

bool clearStaleEdits(backplane::DeviceHost& host, DeviceInstanceId id) {
	std::vector<float> block(pt::kEditsWords);
	if (!host.blockAt(id, pt::kEditsBlock, block)) {
		return false;
	}
	pt::EditState state = pt::editStateOf(block.data(), block.size());
	const size_t present = host.fieldsPresentAt(id);
	const auto fieldAt = [&](size_t i) {
		return i < present ? host.fieldAt(id, i) : static_cast<float>(pt::kParamDescs[i].def);
	};
	bool cleared = false;
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		if (pt::editsStale(state[p], p, fieldAt)) {
			state[p] = pt::EditOverlay {};
			cleared = true;
		}
	}
	if (cleared) {
		pt::writeEditState(state, block.data());
		(void)host.setBlock(id, pt::kEditsBlock, block);
	}
	return cleared;
}

namespace {

/// ONE GESTURE ON THE LANE, AS ONE UNDO STEP: the edits block before and after, written whole either way.
class EditsRecord final : public EditRecord {
public:
	EditsRecord(backplane::DeviceHost& host, DeviceInstanceId id, std::vector<float> before, std::vector<float> after)
	    : host_(&host), id_(id), before_(std::move(before)), after_(std::move(after)) {}
	void undo() const override { (void)host_->setBlock(id_, pt::kEditsBlock, before_); }
	void redo() const override { (void)host_->setBlock(id_, pt::kEditsBlock, after_); }
	const char* verb() const override { return "Pattern edit"; }

private:
	backplane::DeviceHost* host_;
	DeviceInstanceId id_;
	std::vector<float> before_;
	std::vector<float> after_;
};

} // namespace

PatternLane::PatternLane(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	// **SELECT, THEN SET** (a single click selects, a double click adds or deletes): a click selects the
	// step (the Engine rail beside the lane sets it); a double-click adds a note or deletes the one there (in Drum, toggles
	// the hit); a drag on a note sets its pitch, or from its right end its length. No modifier edits a step.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!editable_ || cycle_.length == 0 || e.shouldTriggerPopup()) {
			return;
		}
		// A MARKER'S HANDLE (design §7.8): the press drags that marker, one gesture.
		if (e.position.y <= kMarkHandle) {
			if (const int m = markAt(e.position.x); m != 0) {
				// ARMED AND ROUTABLE: the handle routes (vertical travel is the amount); Alt drags the marker as ever.
				DeviceControl& ctl = m == 1 ? *startCtl_ : *endCtl_;
				const device::ModDisplay d = ctl.display();
				if (!e.isAltDown() && d.isDestination && d.routable) {
					if (e.repeatClickCount() == 2) {
						if (ctl.dropRoute()) {
							redraw();
						}
						return;
					}
					routeMark_ = m;
					routeFromY_ = e.position.y;
					ctl.beginAmountGesture();
					routeAnchor_ = d.amount != 0.0f ? d.amount : kRouteSeedAmount;
					ctl.setAmount(routeAnchor_);
					redraw();
					return;
				}
				markDrag_ = m;
				(m == 1 ? startCtl_ : endCtl_)->beginGesture();
				return;
			}
		}
		press_ = Press {};
		press_.before.assign(pt::kEditsWords, 0.0f);
		if (!host_.blockAt(device_, pt::kEditsBlock, press_.before)) {
			pt::writeEditState(pt::EditState {}, press_.before.data());
		}
		const bool twice = e.repeatClickCount() >= 2;
		if (drum_) {
			const size_t k = stepAt(e.position.x);
			const size_t lane = laneAt(e.position.y);
			if (twice) {
				pt::EditState state = editsNow();
				pn::StepEdit& ed = editAt(state, k);
				const auto bit = static_cast<uint8_t>(1u << lane);
				ed.drumHits = static_cast<uint8_t>(ed.drumHits ^ bit);
				ed.drumAccent = static_cast<uint8_t>(ed.drumAccent & ed.drumHits); // a hit gone takes its accent with it
				writeEdits(state);
				recordEdits(press_.before);
			}
			select(static_cast<int>(k), lane);
			return;
		}
		// THE NOTE UNDER THE HAND, by its bar — a press on a body is a press on its note, never on the step under it.
		const int hit = noteAt(e.position);
		if (twice) {
			pt::EditState state = editsNow();
			const size_t k = hit >= 0 ? static_cast<size_t>(hit) : stepAt(e.position.x);
			pn::StepEdit& ed = editAt(state, k);
			if (hit >= 0) {
				ed.kind = pn::PatternStep::Kind::Rest; // DELETE the note there
			}
			else {
				ed.kind = pn::PatternStep::Kind::Note; // ADD one, at the key under the hand
				pn::setEditKey(ed, terrain_, keyAt(e.position.y));
			}
			writeEdits(state);
			recordEdits(press_.before);
			select(static_cast<int>(k));
			return;
		}
		if (hit < 0) {
			select(static_cast<int>(stepAt(e.position.x))); // AN EMPTY STEP: selected, nothing to drag
			return;
		}
		// A NOTE: selected, and a drag from here sets its pitch — or, from its bar's right end, its length.
		select(hit);
		press_.active = true;
		press_.step = static_cast<size_t>(hit);
		press_.at = e.position;
		const pn::PatternStep& st = cycle_.steps[press_.step];
		const float end = xOfStep(press_.step) + st.gate * (xOfStep(1) - xOfStep(0));
		press_.gate = std::fabs(e.position.x - end) <= 4.0f && e.position.x > xOfStep(press_.step) + 2.0f;
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (routeMark_ != 0) {
			DeviceControl& ctl = routeMark_ == 1 ? *startCtl_ : *endCtl_;
			ctl.setAmount(std::clamp(routeAnchor_ + (routeFromY_ - e.position.y) / kRouteDragRange, -device::kMaxAmount,
			                         device::kMaxAmount));
			redraw();
			return;
		}
		if (markDrag_ != 0) {
			const float column = std::max(1.0f, xOfStep(1) - xOfStep(0));
			const auto len = static_cast<int>(cycle_.length);
			int step = static_cast<int>(std::lround((e.position.x - leftPad()) / column));
			// KEPT IN ORDER: Start before End (a fill's), each inside the cycle.
			if (markDrag_ == 1) {
				step = std::clamp(step, 0, std::max(0, (fill_ ? endMark_ : len) - 1));
				startCtl_->setValue(static_cast<double>(step));
			}
			else {
				step = std::clamp(step, startMark_ + 1, std::max(startMark_ + 1, len));
				endCtl_->setValue(static_cast<double>(step));
			}
			(void)poll();
			return;
		}
		if (!press_.active) {
			return;
		}
		if (!press_.dragged && std::hypot(e.position.x - press_.at.x, e.position.y - press_.at.y) < 3.0f) {
			return;
		}
		press_.dragged = true;
		pt::EditState state = editsNow();
		pn::StepEdit& ed = editAt(state, press_.step);
		if (press_.gate) {
			const float column = xOfStep(1) - xOfStep(0);
			ed.gate = std::clamp((e.position.x - xOfStep(press_.step)) / std::max(1.0f, column), 0.05f, 16.0f);
		}
		else {
			pn::setEditKey(ed, terrain_, keyAt(e.position.y));
		}
		writeEdits(state);
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routeMark_ != 0) {
			(routeMark_ == 1 ? startCtl_ : endCtl_)->endAmountGesture();
			routeMark_ = 0;
			redraw();
			return;
		}
		if (markDrag_ != 0) {
			(markDrag_ == 1 ? startCtl_ : endCtl_)->endGesture();
			markDrag_ = 0;
			return;
		}
		if (!press_.active) {
			return;
		}
		press_.active = false;
		if (press_.dragged) {
			recordEdits(press_.before); // A CLICK ALONE ONLY SELECTS: nothing to record
		}
	};
	// LAST, after every press handler is assigned: `onMouseDown() = ...` replaces the list the hover hook adds to.
	bindMarks();
	installTooltipHover(*this, *this);
}

void PatternLane::bindMarks() {
	// THE FOCUSED TAB'S START AND END, as device controls (one gesture a drag, recorded as the window's other controls are).
	startCtl_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(focus_, Field::StartPhase), mod_,
	                                            history_);
	endCtl_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(focus_, Field::EndPhase), mod_,
	                                          history_);
}

void PatternLane::setModState(ModState* mod) {
	mod_ = mod;
	bindMarks();
	redraw();
}

ControlFace::ModLook PatternLane::modLookFor(const device::Modulatable& c) const {
	const bool start = startCtl_ != nullptr && &c == startCtl_.get();
	const bool end = endCtl_ != nullptr && &c == endCtl_.get() && fill_; // End is drawn only on a fill
	if (!editable_ || (!start && !end)) {
		return {};
	}
	const device::ModDisplay d = c.display();
	if (!d.isDestination) {
		return {};
	}
	return { true, d.armed && d.routable, !d.armed && (d.amount != 0.0f || d.routedElsewhere) };
}

void PatternLane::refreshWatch() {
	ModState* state = startCtl_ != nullptr ? routingStateOf(*startCtl_) : nullptr;
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

void PatternLane::offeredControls(std::vector<Offered>& out) const {
	if (!editable_ || startCtl_ == nullptr) {
		return; // THE CARD'S LANE IS A VIEW
	}
	out.push_back({ startCtl_.get(), Road::Gesture });
	out.push_back({ endCtl_.get(), Road::Gesture }); // OFFERED ALWAYS (what the surface holds); DRAWN only on a fill
}

float PatternLane::markX(int step) const {
	return xOfStep(static_cast<size_t>(std::max(0, step)));
}

int PatternLane::markAt(float x) const {
	if (!editable_ || cycle_.length == 0) {
		return 0;
	}
	const float toEnd = fill_ ? std::fabs(x - markX(endMark_)) : 1e9f;
	const float toStart = std::fabs(x - markX(startMark_));
	if (toEnd <= kMarkReach && toEnd < toStart) {
		return 2;
	}
	return toStart <= kMarkReach ? 1 : 0;
}

void PatternLane::select(int step, size_t lane) {
	const int want = step >= 0 && static_cast<size_t>(step) < cycle_.length ? step : -1;
	const size_t wantLane = std::min(lane, pn::kDrumLanes - 1);
	if (want != selected_ || wantLane != selectedLane_) {
		selected_ = want;
		selectedLane_ = wantLane;
		redraw();
	}
	if (onSelect) {
		onSelect();
	}
}

int PatternLane::noteAt(visage::Point at) const {
	if (drum_ || cycle_.length == 0) {
		return -1;
	}
	const auto h = static_cast<float>(height());
	const float pad = 3.0f;
	const float column = xOfStep(1) - xOfStep(0);
	const float barHeight = std::max(2.0f, (h - 2.0f * pad) / 16.0f);
	// FROM THE STEP UNDER THE HAND BACK: the latest note whose bar reaches `at`, on one of its keys' rows.
	const size_t under = stepAt(at.x);
	for (size_t back = 0; back <= under; ++back) {
		const size_t k = under - back;
		const pn::PatternStep& st = cycle_.steps[k];
		if (st.kind != pn::PatternStep::Kind::Note) {
			continue;
		}
		const float x = xOfStep(k);
		const float bw = std::max(2.0f, st.gate * column);
		if (at.x < x - 1.0f || at.x > x + bw + 1.0f) {
			continue;
		}
		for (int i = 0; i < std::max<int>(1, st.count); ++i) {
			const int key = i == 0 ? cycle_.keyAt(k, 0) : st.keys[static_cast<size_t>(i)];
			const float y = yOfKey(key);
			if (at.y >= y - 2.0f && at.y <= y + barHeight + 2.0f) {
				return static_cast<int>(k);
			}
		}
	}
	return -1;
}

const pn::PatternStep* PatternLane::selectedStepNow() const {
	return selected_ >= 0 && static_cast<size_t>(selected_) < cycle_.length ? &cycle_.steps[static_cast<size_t>(selected_)]
	                                                                         : nullptr;
}

double PatternLane::selectedCellLevel() const {
	const pn::PatternStep* st = selectedStepNow();
	if (st == nullptr) {
		return 0.0;
	}
	const size_t l = std::min(selectedLane_, pn::kDrumLanes - 1);
	if (!modulation_) {
		return static_cast<double>(st->drumVelocity[l]);
	}
	// MODULATION: the lane's level at this step, held from its last hit — the engine's own reading (`pat::modLevelsOf`).
	pt::ModLevels levels {};
	uint8_t live = 0;
	pt::modLevelsOf(cycle_, 0xFF, levels, &live);
	return static_cast<double>(levels[static_cast<size_t>(selected_)][l]);
}

bool PatternLane::modBipolar() const {
	return host_.fieldAt(device_, pt::at(focus_, Field::ModPolarity)) >= 0.5f;
}

int PatternLane::selectedKey() const {
	const pn::PatternStep* st = selectedStepNow();
	if (st == nullptr) {
		return 60;
	}
	return st->kind == pn::PatternStep::Kind::Note ? cycle_.keyAt(static_cast<size_t>(selected_), 0)
	                                               : defaultKeyAt(static_cast<size_t>(selected_));
}

int PatternLane::defaultKeyAt(size_t step) const {
	const pn::PatternStep& t = terrain_.steps[std::min(step, terrain_.steps.size() - 1)];
	return t.kind == pn::PatternStep::Kind::Note ? terrain_.keyAt(step, 0) : terrain_.rootKey;
}

int PatternLane::snapKey(int raw, int from) const {
	const size_t present = host_.fieldsPresentAt(device_);
	const auto fieldAt = [&](size_t i) {
		return i < present ? host_.fieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	};
	const auto scale = mod::arp::theory::scaleOf(static_cast<size_t>(std::max(0L, std::lround(fieldAt(pt::at(focus_, Field::Scale))))));
	const int tonic = 48 + static_cast<int>(std::lround(fieldAt(pt::at(focus_, Field::Root))));
	int key = std::clamp(mod::arp::theory::snapToScale(scale, tonic, raw), 0, 127);
	// A NUDGE OF ONE SEMITONE THAT SNAPS BACK WHERE IT WAS GOES ON TO THE NEXT DEGREE THAT WAY, so a drag never sticks.
	if (key == from && raw != from) {
		const int dir = raw > from ? 1 : -1;
		for (int k = from + dir; k >= 0 && k <= 127; k += dir) {
			if (mod::arp::theory::snapToScale(scale, tonic, k) == k) {
				key = k;
				break;
			}
		}
	}
	return key;
}

void PatternLane::beginStepEdit() {
	if (stepGestures_++ == 0) {
		stepBefore_.assign(pt::kEditsWords, 0.0f);
		if (!host_.blockAt(device_, pt::kEditsBlock, stepBefore_)) {
			pt::writeEditState(pt::EditState {}, stepBefore_.data());
		}
	}
}

void PatternLane::editSelected(const std::function<void(pn::StepEdit&)>& fn) {
	if (selected_ < 0 || static_cast<size_t>(selected_) >= cycle_.length) {
		return;
	}
	const bool own = stepGestures_ == 0; // A WRITE WITH NO GESTURE OPEN (a typed value) is a step of its own
	if (own) {
		beginStepEdit();
	}
	pt::EditState state = editsNow();
	fn(editAt(state, static_cast<size_t>(selected_)));
	writeEdits(state);
	if (own) {
		endStepEdit();
	}
}

void PatternLane::endStepEdit() {
	if (stepGestures_ > 0 && --stepGestures_ == 0) {
		recordEdits(stepBefore_);
	}
}

const char* PatternLane::tooltipAt(visage::Point at) const {
	if (!editable_) {
		return nullptr; // THE CARD'S LANE IS A VIEW: it has no gesture to name
	}
	// A MARKER'S HANDLE names its control, in the control's own words.
	if (at.y <= kMarkHandle) {
		if (const int m = markAt(at.x); m != 0) {
			return pt::kParamDescs[pt::at(focus_, m == 1 ? Field::StartPhase : Field::EndPhase)].tooltip;
		}
	}
	return drum_ ? kDrumGestures : kPitchedGestures;
}

void PatternLane::setEditable(bool editable, EditHistory* history) {
	editable_ = editable;
	history_ = history;
	bindMarks();
}

pt::EditState PatternLane::editsNow() const {
	std::vector<float> block(pt::kEditsWords);
	return host_.blockAt(device_, pt::kEditsBlock, block) ? pt::editStateOf(block.data(), block.size()) : pt::EditState {};
}

void PatternLane::writeEdits(const pt::EditState& state) {
	std::vector<float> block(pt::kEditsWords);
	pt::writeEditState(state, block.data());
	(void)host_.setBlock(device_, pt::kEditsBlock, block);
	(void)poll();
}

void PatternLane::recordEdits(const std::vector<float>& before) {
	std::vector<float> after(pt::kEditsWords);
	if (!host_.blockAt(device_, pt::kEditsBlock, after) || after == before || history_ == nullptr) {
		return;
	}
	history_->record(std::make_unique<EditsRecord>(host_, device_, before, after));
}

pn::StepEdit& PatternLane::editAt(pt::EditState& state, size_t k) const {
	pt::EditOverlay& o = state[focus_];
	const size_t present = host_.fieldsPresentAt(device_);
	const auto fieldAt = [&](size_t i) {
		return i < present ? host_.fieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	};
	const auto mode = static_cast<pt::Mode>(std::clamp<long>(std::lround(fieldAt(pt::at(focus_, Field::Mode))), 0,
	                                                         static_cast<long>(pt::Mode::kCount) - 1));
	// AN OVERLAY IS ANCHORED AT THE PLACE AND MODE IT WAS MADE UNDER; the first edit here anchors it.
	if (!o.any()) {
		o = pt::EditOverlay {};
		o.anchor = pt::anchorNow(focus_, fieldAt);
		o.mode = mode;
	}
	pn::StepEdit& ed = o.steps[std::min(k, o.steps.size() - 1)];
	if (!ed.on) {
		ed = pn::editOf(terrain_.steps[std::min(k, terrain_.steps.size() - 1)], drum_);
	}
	return ed;
}

size_t PatternLane::stepAt(float x) const {
	if (cycle_.length == 0) {
		return 0;
	}
	const float pad = leftPad();
	const float column = (static_cast<float>(width()) - pad - 3.0f) / static_cast<float>(cycle_.length);
	return static_cast<size_t>(std::clamp((x - pad) / std::max(1.0f, column), 0.0f, static_cast<float>(cycle_.length - 1)));
}

float PatternLane::xOfStep(size_t k) const {
	const float pad = leftPad();
	const float column = (static_cast<float>(width()) - pad - 3.0f) / static_cast<float>(std::max<size_t>(cycle_.length, 1));
	return pad + column * static_cast<float>(k);
}

size_t PatternLane::laneAt(float y) const {
	const float pad = 3.0f;
	const float row = (static_cast<float>(height()) - 2.0f * pad) / static_cast<float>(pn::kDrumLanes);
	const float fromFoot = static_cast<float>(height()) - pad - y;
	return static_cast<size_t>(std::clamp(fromFoot / std::max(1.0f, row), 0.0f, static_cast<float>(pn::kDrumLanes - 1)));
}

float PatternLane::yOfLane(size_t lane) const {
	const float pad = 3.0f;
	const float row = (static_cast<float>(height()) - 2.0f * pad) / static_cast<float>(pn::kDrumLanes);
	return static_cast<float>(height()) - pad - row * static_cast<float>(lane + 1);
}

float PatternLane::yOfKey(int key) const {
	const auto h = static_cast<float>(height());
	const float pad = 3.0f;
	const float barHeight = std::max(2.0f, (h - 2.0f * pad) / 16.0f);
	return h - pad - barHeight - static_cast<float>(key - lo_) / span_ * (h - 2.0f * pad - barHeight);
}

std::vector<int> PatternLane::tonicKeys() const {
	std::vector<int> keys;
	if (drum_ || cycle_.length == 0) {
		return keys;
	}
	// THE VIEW'S KEYS ARE lo_ .. lo_ + span_ (`yOfKey` puts lo_ on the floor and lo_ + span_ on the ceiling).
	const int top = lo_ + static_cast<int>(std::floor(span_));
	const int first = lo_ + ((cycle_.rootKey - lo_) % 12 + 12) % 12;
	for (int key = first; key <= top; key += 12) {
		keys.push_back(key);
	}
	return keys;
}

int PatternLane::keyAt(float y) const {
	// THE KEY UNDER THE HAND, SNAPPED TO THE FOCUSED PATTERN'S SCALE (on its tonic: the Root under Pool, C otherwise).
	const auto h = static_cast<float>(height());
	const float pad = 3.0f;
	const float barHeight = std::max(2.0f, (h - 2.0f * pad) / 16.0f);
	const float u = (h - pad - barHeight - y) / std::max(1.0f, h - 2.0f * pad - barHeight);
	const int raw = static_cast<int>(std::lround(static_cast<float>(lo_) + u * span_));
	const size_t present = host_.fieldsPresentAt(device_);
	const auto fieldAt = [&](size_t i) {
		return i < present ? host_.fieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	};
	const auto scale = static_cast<size_t>(std::max(0L, std::lround(fieldAt(pt::at(focus_, Field::Scale)))));
	const int root = static_cast<int>(std::lround(fieldAt(pt::at(focus_, Field::Root))));
	return std::clamp(mod::arp::theory::snapToScale(mod::arp::theory::scaleOf(scale), 48 + root, raw), 0, 127);
}

void PatternLane::setFocus(size_t pattern) {
	focus_ = std::min(pattern, pt::kPatterns - 1);
	bindMarks();
	if (focus_ != lastFocus_) {
		selected_ = -1; // THE ACTIVE STEP IS THE FOCUSED PATTERN'S
	}
}

bool PatternLane::pollPlayhead() {
	// THE DEVICE'S PLAYHEAD (`PatternDevice::playhead`: pattern << 6 | step, idle when nothing plays) — the strip's own
	// road, as every other strip visual's playhead.
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase)) {
		ordinal = mod::Playhead::kIdle;
	}
	int step = -1;
	if (ordinal != mod::Playhead::kIdle && (ordinal >> pt::PatternDevice::kPlayheadStepBits) == focus_) {
		step = static_cast<int>(ordinal & ((1u << pt::PatternDevice::kPlayheadStepBits) - 1u));
	}
	if (step == playStep_) {
		return false;
	}
	playStep_ = step;
	redraw();
	return true;
}

bool PatternLane::poll() {
	const bool moved = pollPlayhead();
	// ONE MEMBER BUFFER holds the row, the hand's words and the edits, in that order: sized once, refilled in place, so a
	// tick at rest allocates nothing.
	std::vector<float>& row = rowBuf_;
	row.resize(pt::kParamCount + pt::kHandWords + pt::kEditsWords);
	const size_t present = host_.fieldsPresentAt(device_);
	for (size_t i = 0; i < pt::kParamCount; ++i) {
		row[i] = i < present ? host_.modulatedFieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	}
	// THE HAND'S BLOCK IS AN INPUT TOO (a Rotate fold, a pin switch): appended to what the lane compares.
	const std::span<float> handWords(row.data() + pt::kParamCount, pt::kHandWords);
	std::fill(handWords.begin(), handWords.end(), 0.0f);
	(void)host_.blockAt(device_, 1, handWords);
	// AND THE HAND'S EDITS.
	const std::span<float> editWords(row.data() + pt::kParamCount + pt::kHandWords, pt::kEditsWords);
	std::fill(editWords.begin(), editWords.end(), 0.0f);
	const bool haveEdits = host_.blockAt(device_, pt::kEditsBlock, editWords);
	if (row == lastRow_ && focus_ == lastFocus_) {
		return moved;
	}
	lastRow_ = row; // same size as before: assigned in place
	lastFocus_ = focus_;
	// THE DEVICE'S OWN ROAD, over a held C4 and every pin switched on (the switches live in the hand's block; a pattern
	// whose switches are off draws as if they were on — the lane is its shape, the sound is the device's).
	const pt::Settings s = pt::settingsOf(row.data(), row.size());
	mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	std::vector<float> block(pt::kHandWords);
	const pt::HandState hand = host_.blockAt(device_, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	pn::Recipe recipe = pt::recipeFor(s, focus_, hand[focus_].pinOn);
	drum_ = mod::arp::genes::decodesDrum(recipe.mode);
	modulation_ = mod::arp::genes::isModulation(recipe.mode);
	progression_ = mod::arp::genes::isProgression(recipe.mode);
	drumOn_ = recipe.drum.on;
	fill_ = s.phase[focus_].fill;
	pn::build(pt::PatternDevice::placeOf(s.patterns[focus_], hand[focus_].residue), recipe, held, terrain_);
	cycle_ = terrain_;
	// THE HAND'S EDITS, as the device applies them: under the Mode they were made in, on the steps that still match.
	if (haveEdits) {
		const pt::EditState edits = pt::editStateOf(editWords.data(), editWords.size());
		if (edits[focus_].mode == recipe.mode) {
			pn::applyEdits(edits[focus_].steps, drum_, cycle_, modulation_);
		}
	}
	// THE PITCH SPAN THE DRAWING AND THE HAND SHARE (`yOfKey`, `keyAt`).
	int lo = 127;
	int hi = 0;
	for (size_t k = 0; k < cycle_.length; ++k) {
		const pn::PatternStep& st = cycle_.steps[k];
		if (st.kind != pn::PatternStep::Kind::Note) {
			continue;
		}
		for (int i = 0; i < st.count; ++i) {
			const int key = i == 0 ? cycle_.keyAt(k, 0) : st.keys[static_cast<size_t>(i)];
			lo = std::min(lo, key);
			hi = std::max(hi, key);
		}
	}
	lo_ = lo > hi ? 48 : lo - 2;
	span_ = static_cast<float>(lo > hi ? 24 : std::max(hi - lo + 4, 12));
	redraw();
	// THE MARKERS, in the cycle: Start inside it, a fill's End past Start (the device reads them so; design §7.8).
	{
		const int len = std::max(1, static_cast<int>(cycle_.length));
		startMark_ = std::clamp(s.phase[focus_].start, 0, len - 1);
		endMark_ = std::clamp(s.phase[focus_].end, startMark_ + 1, len);
	}
	// THE ACTIVE STEP: gone with a cycle too short for it; and the Engine rail re-reads what the step now holds.
	if (selected_ >= 0 && static_cast<size_t>(selected_) >= cycle_.length) {
		selected_ = -1;
	}
	if (onSelect) {
		onSelect();
	}
	return true;
}

void PatternLane::draw(visage::Canvas& canvas) {
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	if (cycle_.length == 0) {
		return;
	}
	// THE PLAYHEAD: the step playing now, a column in the ink at a tenth (the step sequencer's), under the notes.
	if (playStep_ >= 0 && static_cast<size_t>(playStep_) < cycle_.length) {
		const float column = xOfStep(1) - xOfStep(0);
		canvas.setColor(withAlpha(kInk, 0.10f));
		canvas.fill(xOfStep(static_cast<size_t>(playStep_)), 2.0f, column, h - 4.0f);
	}
	if (drum_) {
		drawDrumGrid(canvas);
	}
	else {
		const float pad = 3.0f;
		const float column = (w - 2.0f * pad) / static_cast<float>(cycle_.length);
		const float barHeight = std::max(2.0f, (h - 2.0f * pad) / 16.0f);
		// THE TONIC'S ROWS, a band under the notes in the ink at a sixteenth — fainter than the playhead, so both read.
		canvas.setColor(withAlpha(kInk, 0.06f));
		for (const int key : tonicKeys()) {
			canvas.fill(pad, yOfKey(key), w - 2.0f * pad, barHeight);
		}
		// **PROGRESSION'S CHORDS AS BLOCKS** (design §11-quater): each from its strike to the next chord's, a step's gap
		// between, named by its numeral at the top-left — under the notes, which still show the voicing.
		if (progression_) {
			for (size_t k = 0; k < cycle_.length; ++k) {
				if (!cycle_.steps[k].chordStart) {
					continue;
				}
				size_t end = k + 1;
				while (end < cycle_.length && !cycle_.steps[end].chordStart) {
					++end;
				}
				const float x = pad + column * static_cast<float>(k);
				const float bw = std::max(2.0f, column * static_cast<float>(end - k) - 2.0f);
				canvas.setColor(withAlpha(kMuted, 0.14f));
				canvas.roundedRectangle(x, pad, bw, h - 2.0f * pad, 2.0f);
				char numeral[16];
				mod::arp::pattern::numeralOf(cycle_.steps[k], numeral, sizeof numeral);
				if (textWidth(numeral, font(kKnobLabelSize)) > bw - 3.0f) {
					continue; // A BLOCK TOO NARROW FOR ITS NAME SHOWS NONE, rather than a name over its neighbour's
				}
				canvas.setColor(kMuted);
				canvas.text(numeral, font(kKnobLabelSize), visage::Font::kTopLeft, x + 2.0f, pad + 1.0f, bw - 3.0f,
				            kKnobLabelSize + 2.0f);
			}
		}
		for (size_t k = 0; k < cycle_.length; ++k) {
			const pn::PatternStep& st = cycle_.steps[k];
			if (st.kind != pn::PatternStep::Kind::Note) {
				continue;
			}
			const float x = pad + column * static_cast<float>(k);
			// A NOTE'S BAR RUNS AS LONG AS ITS GATE — through its ties — and an accent is drawn in the accent; its
			// velocity is its strength.
			const float bw = std::max(2.0f, std::min(st.gate * column, w - pad - x) - 1.0f);
			canvas.setColor(withAlpha(st.accent ? kAccent : kMuted, 0.45f + 0.55f * std::clamp(st.velocity, 0.0f, 1.0f)));
			if (st.approach != pn::ApproachKind::None) {
				// A PICKUP (the Approach pin, design §10-ter): an OUTLINED note, so a lead-in reads apart from the chords it
				// walks between.
				canvas.roundedRectangleBorder(x, yOfKey(st.keys[0]), bw, barHeight, 1.0f, 1.0f);
				continue;
			}
			for (int i = 0; i < st.count; ++i) {
				const int key = i == 0 ? cycle_.keyAt(k, 0) : st.keys[static_cast<size_t>(i)];
				canvas.roundedRectangle(x, yOfKey(key), bw, barHeight, 1.0f);
			}
			if (st.slide) {
				// A SLIDE: a tick from the bar's end toward the next step.
				canvas.fill(x + bw, yOfKey(cycle_.keyAt(k, 0)) + barHeight * 0.5f - 0.5f, std::max(1.0f, column - bw), 1.0f);
			}
		}
	}
	// **THE HAND'S MARK** (design §15-bis): every step an edit holds, a dot in the value colour over its column.
	const float pad = leftPad();
	const float column = (w - pad - 3.0f) / static_cast<float>(cycle_.length);
	canvas.setColor(kValue);
	for (size_t k = 0; k < cycle_.length; ++k) {
		if (cycle_.steps[k].edited) {
			canvas.circle(pad + column * (static_cast<float>(k) + 0.5f) - 2.0f, 1.0f, 4.0f);
		}
	}
	// **THE PHASE MARKERS** (design §7.8), the window's alone: Start in the value colour; a fill's End in the accent, the
	// steps outside its window veiled (the pattern under it plays there). Each a line with a handle in the top band.
	if (editable_) {
		const float top = 1.0f;
		if (fill_) {
			canvas.setColor(withAlpha(kVoid, 0.55f));
			const float x0 = markX(startMark_);
			const float x1 = markX(endMark_);
			canvas.fill(pad, 2.0f, std::max(0.0f, x0 - pad), h - 4.0f);
			canvas.fill(x1, 2.0f, std::max(0.0f, w - 3.0f - x1), h - 4.0f);
		}
		refreshWatch();
		const auto marker = [&](int step, uint32_t colour, const DeviceControl* ctl) {
			const float x = markX(step);
			canvas.setColor(colour);
			canvas.fill(x - 0.75f, top, 1.5f, h - 2.0f);
			canvas.roundedRectangle(x - 4.0f, top, 8.0f, kMarkHandle - 2.0f, 2.0f);
			// ITS MODULATION: armed and routable, the handle glows; routed, a ring beside it.
			const ModLook look = ctl != nullptr ? modLookFor(*ctl) : ModLook {};
			if (look.armedGlow) {
				canvas.setColor(withAlpha(kModulation, 0.45f));
				canvas.roundedRectangle(x - 6.0f, top - 1.0f, 12.0f, kMarkHandle, 3.0f);
				canvas.setColor(kModulation);
				canvas.roundedRectangleBorder(x - 6.0f, top - 1.0f, 12.0f, kMarkHandle, 3.0f, 1.0f);
			}
			else if (look.routedMark) {
				canvas.setColor(withAlpha(kModulation, 0.9f));
				canvas.ring(x + 5.0f, top + 1.0f, 6.0f, 1.0f);
			}
		};
		marker(startMark_, kValue, startCtl_.get());
		if (fill_) {
			marker(endMark_, kAccent, endCtl_.get());
		}
	}
	// **THE ACTIVE STEP**, outlined in the accent: its column, or in Drum its cell.
	if (editable_ && selected_ >= 0 && static_cast<size_t>(selected_) < cycle_.length) {
		const float x = pad + column * static_cast<float>(selected_);
		canvas.setColor(kAccent);
		if (drum_) {
			const float row = (h - 6.0f) / static_cast<float>(pn::kDrumLanes);
			canvas.roundedRectangleBorder(x - 0.5f, yOfLane(selectedLane_) - 0.5f, column + 1.0f, row + 1.0f, 2.0f, 1.5f);
		}
		else {
			canvas.roundedRectangleBorder(x, 2.0f, std::max(2.0f, column), h - 4.0f, 2.0f, 1.5f);
		}
	}
}

void PatternLane::drawDrumGrid(visage::Canvas& canvas) {
	// **THE DRUM GRID** (design §11-ter): one row a lane, kick at the bottom; a hit a cell, an accent in the accent colour,
	// a ghost faint, a roll split into its strokes; the bar lines every sixteen steps.
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	const float pad = leftPad();
	const float column = (w - pad - 3.0f) / static_cast<float>(cycle_.length);
	const float row = (h - 6.0f) / static_cast<float>(pn::kDrumLanes);
	// THE ROWS NAMED AT THE LEFT EDGE (K, S, CH, OH, Cl, LT, HT, R — the lanes' names, kick at the foot); a lane switched
	// off is dimmed, its label and its row.
	static constexpr const char* kLabels[pn::kDrumLanes] = { "K", "S", "CH", "OH", "Cl", "LT", "HT", "R" };
	// MODULATION'S ROWS ARE NOT THE KIT'S (the mod pattern view must not reuse the drum names): M1–M8 in the gutter, Mod 1–8 wherever there is room.
	static constexpr const char* kModLabels[pn::kDrumLanes] = { "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8" };
	for (size_t l = 0; l < pn::kDrumLanes; ++l) {
		const float y = h - 3.0f - row * static_cast<float>(l + 1);
		const bool on = ((drumOn_ >> l) & 1u) != 0u;
		if (!on) {
			canvas.setColor(withAlpha(kSurfaceRaised, 0.6f));
			canvas.fill(pad, y, w - pad - 3.0f, row);
		}
		canvas.setColor(on ? kMuted : withAlpha(kMuted, 0.4f));
		canvas.text(modulation_ ? kModLabels[l] : kLabels[l], font(std::min(kKnobLabelSize, row)), visage::Font::kCenter, 2.0f, y, kDrumGutter, row);
	}
	canvas.setColor(withAlpha(kLine, 0.5f));
	for (size_t k = 16; k < cycle_.length; k += 16) {
		canvas.fill(pad + column * static_cast<float>(k), 3.0f, 1.0f, h - 6.0f);
	}
	for (size_t k = 0; k < cycle_.length; ++k) {
		const pn::PatternStep& st = cycle_.steps[k];
		for (size_t l = 0; l < pn::kDrumLanes; ++l) {
			if (((st.drumHits >> l) & 1u) == 0u) {
				continue;
			}
			const float x = pad + column * static_cast<float>(k);
			const float y = h - 3.0f - row * static_cast<float>(l + 1);
			const bool accent = ((st.drumAccent >> l) & 1u) != 0u;
			const bool ghost = ((st.drumGhost >> l) & 1u) != 0u;
			canvas.setColor(accent ? kAccent : (ghost ? withAlpha(kMuted, 0.45f) : kMuted));
			const float cw = std::max(2.0f, column - 1.0f);
			const float ch = std::max(2.0f, row - 1.0f);
			if (((st.drumRoll >> l) & 1u) != 0u && st.ratchet > 1) {
				const float part = cw / static_cast<float>(st.ratchet);
				for (int j = 0; j < st.ratchet; ++j) {
					canvas.fill(x + part * static_cast<float>(j), y, std::max(1.0f, part - 1.0f), ch);
				}
			}
			else {
				canvas.roundedRectangle(x, y, cw, ch, 1.0f);
			}
		}
	}
}

//------------------------------------------------------------------------------------------------//
// The tabs                                                                                       //
//------------------------------------------------------------------------------------------------//

PatternTabs::PatternTabs() {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		for (size_t i = 0; i < pt::kPatterns; ++i) {
			if (tabBounds(i).contains(e.position) && e.shouldTriggerPopup()) {
				if (onMenu) {
					onMenu(i, e.position); // THE TAB MENU (Drawn's frame tabs' verbs)
				}
				return;
			}
			if (tabBounds(i).contains(e.position) && onFocus) {
				onFocus(i);
				return;
			}
		}
	};
	// LAST, as `installTooltipHover` asks: its hooks run beside the press above.
	installTooltipHover(*this, *this);
}

visage::Bounds PatternTabs::numberBounds(size_t i) const {
	const float cell = static_cast<float>(width()) / static_cast<float>(pt::kPatterns);
	// THE NUMBER'S ROW is the strip's first: a strip made taller for Cycle's cells (the window's) keeps it there.
	const float h = std::min(static_cast<float>(height()), kNumberRow);
	return { cell * static_cast<float>(i), 0.0f, std::min(cell, h), h };
}

visage::Bounds PatternTabs::tabBounds(size_t i) const {
	const float cell = static_cast<float>(width()) / static_cast<float>(pt::kPatterns);
	return { cell * static_cast<float>(i), 0.0f, cell, static_cast<float>(height()) };
}

uint32_t PatternTabs::tabFill() {
	return kSurfaceRaised;
}

void PatternTabs::drawPanel(visage::Canvas& canvas, float x, float y, float w, float h) {
	canvas.setColor(tabFill());
	canvas.fill(x, y, w, h);
	canvas.setColor(kLine);
	canvas.fill(x, y, 1.0f, h);
	canvas.fill(x + w - 1.0f, y, 1.0f, h);
	canvas.fill(x, y + h - 1.0f, w, 1.0f);
}

void PatternTabs::draw(visage::Canvas& canvas) {
	const float cell = static_cast<float>(width()) / static_cast<float>(pt::kPatterns);
	const auto h = static_cast<float>(height());
	constexpr float kRound = 4.0f;
	// THE RULE THE TABS STAND ON — the panel's head; open under the focused tab, which joins the panel.
	canvas.setColor(kLine);
	canvas.fill(0.0f, h - 1.0f, static_cast<float>(width()), 1.0f);
	for (size_t i = 0; i < pt::kPatterns; ++i) {
		const float x = cell * static_cast<float>(i);
		if (i == focus_) {
			// RAISED AND OPEN AT ITS FOOT: the panel's fill, ruled on three sides, running past the frame's foot so the
			// rounding is clipped away there and the tab and the panel are one surface.
			canvas.setColor(kLine);
			canvas.roundedRectangle(x, 0.0f, cell, h + kRound, kRound);
			canvas.setColor(tabFill());
			canvas.roundedRectangle(x + 1.0f, 1.0f, cell - 2.0f, h + kRound, kRound - 1.0f);
			// AND MARKED: the accent along its head, the one tab that has it.
			canvas.setColor(kAccent);
			canvas.roundedRectangle(x + 1.0f, 1.0f, cell - 2.0f, 2.0f, 1.0f);
		}
		else {
			// RECESSED: lower, darker, faintly ruled, behind the strip's rule.
			canvas.setColor(withAlpha(kLine, 0.6f));
			canvas.roundedRectangle(x + 2.0f, 4.0f, cell - 4.0f, h - 4.0f, kRound);
			canvas.setColor(kVoid);
			canvas.roundedRectangle(x + 3.0f, 5.0f, cell - 6.0f, h - 6.0f, kRound - 1.0f);
		}
		const visage::Bounds n = numberBounds(i);
		canvas.setColor(i == focus_ ? kInk : kMuted);
		canvas.text(std::to_string(i + 1), font(kKnobReadoutSize), visage::Font::kCenter, n.x(), n.y(), n.width(), n.height());
		// THE PATTERN THE SELECTOR PICKS NOW: a dot, not a control (design §15).
		if (i == playing_) {
			canvas.setColor(kAccent);
			canvas.circle(n.x() + n.width() - 6.0f, n.y() + 3.0f, 3.0f);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// The card                                                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

/// **THE SELECTOR'S CARD READOUT, COMPACT** — "1 · C Maj": the pattern it picks, its root (under Pool; Transpose takes it
/// from the key) and the scale's first syllables. The DAW and the tooltip keep the full words (`pattern_text.cpp`).
class CompactSelector final : public DeviceControl {
public:
	CompactSelector(backplane::DeviceHost& host, DeviceInstanceId id, ModState* mod, EditHistory* history)
	    : DeviceControl(host, id, pt::kDescriptor, pt::at(Global::Selector), mod, history), hostRef_(host) {}
	std::string text() const override {
		std::vector<float> row(pt::kParamCount);
		const size_t present = host().fieldsPresentAt(device());
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = i < present ? host().fieldAt(device(), i) : static_cast<float>(pt::kParamDescs[i].def);
		}
		pt::Settings s = pt::settingsOf(row.data(), row.size());
		s.selector = static_cast<float>(value());
		const size_t p = pt::selected(s);
		if (p >= pt::kPatterns) {
			return "\u2014";
		}
		const pt::PatternSettings& q = s.patterns[p];
		std::string scale;
		const std::string name = mod::arp::theory::kScales[std::min(q.scale, mod::arp::theory::kScaleCount - 1)].name;
		size_t words = 0;
		for (size_t i = 0; i < name.size() && words < 2;) {
			while (i < name.size() && name[i] == ' ') {
				++i;
			}
			const size_t start = i;
			while (i < name.size() && name[i] != ' ') {
				++i;
			}
			if (i > start) {
				scale += (words > 0 ? " " : "") + name.substr(start, std::min<size_t>(3, i - start));
				++words;
			}
		}
		std::string out = std::to_string(p + 1) + " \u00b7 ";
		if (s.keys == pn::Keys::Pool) {
			out += std::string(pt::kRootNames[static_cast<size_t>(((q.root % 12) + 12) % 12)]) + " ";
		}
		return out + scale;
	}

private:
	backplane::DeviceHost& host() const { return hostRef_; }
	backplane::DeviceHost& hostRef_;
};

} // namespace

PatternCard::PatternCard(backplane::DeviceHost& host, WidgetRegistry&, DeviceInstanceId id, Notifier* notifier,
                         ModState* mod, EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	controls_.resize(pt::kDescriptor.params.size());
	tabs_ = std::make_unique<PatternTabs>();
	tabs_->onFocus = [this](size_t p) { setFocus(p); };
	tabs_->onMenu = [this](size_t p, visage::Point at) { showPatternTabMenu(host_, device_, p, history_, *tabs_, at); };
	addChild(tabs_.get());
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		PatternWidgets& w = patterns_[p];
		w.enable = makeModulatedChip(control(pt::at(p, Field::Enable)), "E");
		w.solo = makeModulatedChip(control(pt::at(p, Field::Solo)), "S");
		w.solo->setPressOverride([this, p](const visage::MouseEvent& e) {
			if (pressPatternSolo(host_, device_, p, e.isMainModifier(), history_)) {
				setFocus(p);
			}
			return true;
		});
		w.mode = std::make_unique<ValueBox>(control(pt::at(p, Field::Mode)), "Mode");
		w.scale = std::make_unique<ValueBox>(control(pt::at(p, Field::Scale)), "Scale");
		w.rate = std::make_unique<Knob>(control(pt::at(p, Field::Rate)), "Rate", notifier_);
		w.length = std::make_unique<Knob>(control(pt::at(p, Field::Length)), "Length", notifier_);
		w.next = std::make_unique<ValueBox>(control(pt::at(p, Field::Next)), "Next");
		w.repeats = std::make_unique<ValueBox>(control(pt::at(p, Field::Repeats)), "Repeats");
		addChild(w.enable.get());
		addChild(w.solo.get());
		addChild(w.mode.get());
		addChild(w.scale.get());
		addChild(w.next.get());
		addChild(w.repeats.get());
	}
	keys_ = std::make_unique<ValueBox>(control(pt::at(Global::Keys)), "Keys");
	{
		const size_t mode = pt::at(Global::HumanizeMode);
		std::vector<ZoneSelect::ZoneName> names;
		for (const char* n : pt::kParamDescs[mode].names) {
			names.push_back({ n == nullptr ? "" : n, {} });
		}
		humanize_ = std::make_unique<ModeValueCell>(control(mode), "Humanize", std::move(names),
		                                            [this](size_t param) { return param < pt::kParamCount && live(param); });
		// THE AMOUNT ONLY: Seed is gated on the same mode but is a quantity of its own, not the feel's amount (the cell shows
		// one follower at a time, so two live at once could not both be there); it is the window's.
		humanize_->addFollower(pt::at(Global::HumanizeAmount),
		                       std::make_unique<ValueBox>(control(pt::at(Global::HumanizeAmount)), ""));
		// FREE'S PHASE in the Free feels (design §12-bis): a digit box, as SKLITTER's. Its Fine is the window's.
		humanize_->addFollower(pt::at(Global::FreePhase), std::make_unique<ValueBox>(control(pt::at(Global::FreePhase)), ""));
		humanize_->setFaceDiameter(kFaceDiameter);
	}
	addChild(keys_.get());
	addChild(humanize_.get());
	wobble_ = std::make_unique<ValueBox>(control(pt::at(Global::Wobble)), "Wobble");
	addChild(wobble_.get());
	latch_ = makeModulatedChip(control(pt::at(Global::Latch)), "Latch");
	addChild(latch_.get());
	cycle_ = makeModulatedChip(control(pt::at(Global::PlayMode)), "Cycle");
	addChild(cycle_.get());
	compactSelector_ = std::make_unique<CompactSelector>(host_, device_, mod_, history_);
	selector_ = std::make_unique<Knob>(*compactSelector_, "Pattern", notifier_);
	lane_ = std::make_unique<PatternLane>(host_, device_);
	addChild(lane_.get());
	for (Knob* knob : knobs()) {
		knob->setFaceDiameter(kFaceDiameter);
		knob->setModulation(mod_, history_);
		addChild(knob);
	}
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	layoutControls();
	showTabOutputs();
}

PatternCard::~PatternCard() = default;

std::vector<Knob*> PatternCard::knobs() const {
	std::vector<Knob*> out { selector_.get() };
	for (const PatternWidgets& w : patterns_) {
		out.push_back(w.rate.get());
		out.push_back(w.length.get());
	}
	return out;
}

DeviceControl& PatternCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

device::Modulatable* PatternCard::enableModulatable() {
	return &control(pt::at(Global::Enabled));
}

void PatternCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		if (controls_[i] != nullptr) {
			controls_[i]->setModulation(mod, history);
		}
	}
	compactSelector_->setModulation(mod, history);
	for (Knob* knob : knobs()) {
		knob->setModulation(mod, history);
	}
}

void PatternCard::showTabOutputs() {
	// **THE CARD SHOWS THE OPEN TAB'S SOURCES** (design §7.10): its gate, and its lanes while it is in Modulation. Every
	// host lays this card's chips by the mask (`DeviceHost::outputsShown`); a route from a hidden one reads on.
	const long mode = std::lround(host_.fieldAt(device_, pt::at(focus_, Field::Mode)));
	const uint64_t mask = pt::cardOutputsFor(focus_, mode == static_cast<long>(pt::Mode::Modulation));
	if (host_.outputsShown(device_) != mask) {
		host_.setOutputsShown(device_, mask);
	}
}

void PatternCard::setFocus(size_t pattern) {
	focus_ = std::min(pattern, pt::kPatterns - 1);
	tabs_->setFocus(focus_);
	lane_->setFocus(focus_);
	showTabOutputs();
	layoutControls();
	redraw();
}

std::vector<size_t> PatternCard::addressedParamsForTest() const {
	std::vector<size_t> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(road->control());
		}
	};
	for (const PatternWidgets& w : patterns_) {
		add(&w.enable->control());
		add(&w.solo->control());
		add(&w.mode->control());
		add(&w.scale->control());
		add(&w.next->control());
		add(&w.repeats->control());
	}
	add(&cycle_->control());
	add(&keys_->control());
	add(&humanize_->select().control());
	for (const size_t j : humanize_->followerParams()) {
		out.push_back(j);
	}
	add(&wobble_->control());
	add(&latch_->control());
	for (const Knob* knob : knobs()) {
		add(knob->control());
	}
	return out;
}

float PatternCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float PatternCard::laneHeightAt(float cardWidth) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	// IN A HORIZONTAL CHAIN the head is the spine at the left, and the body is the compact one (`compactBodyHeight`).
	if (laneAxis() == LaneAxis::Horizontal) {
		return compactBodyHeight() + outputStripHeightAt(cardWidth);
	}
	return kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

void PatternCard::resized() {
	layoutControls();
}

void PatternCard::foldedChanged() {
	layoutControls();
}

void PatternCard::layoutControls() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const bool open = !folded();
	float y = bodyTopEdge() + kGap;
	const float kw = std::max(0.0f, (inner - 2.0f * kRowGap) / 3.0f);

	// **IN A HORIZONTAL CHAIN, ONE UNIT TALL** (the card overlapped its own widgets there) — the Rack
	// card's rule (c0496c20): what the unit has no room for is the window's. Here that
	// is Humanize (its zone cell is in the window), the device's row is ONE knob-height (the selector, and Keys over
	// Latch beside it), and the lane is shorter (`kLaneHeightCompact`).
	const bool compact = laneAxis() == LaneAxis::Horizontal;
	for (Knob* knob : knobs()) {
		knob->setVisible(false);
	}
	// CYCLE (design §7.6): the selector is not read, so it is not drawn (the honesty rule); the focused tab's cycle cell —
	// its Next over its Repeats — takes its place. Every tab's cell is the window's (on its tab).
	const bool cycling = !live(pt::at(Global::Selector));
	const auto seatCycle = [&](float x, float top, float w, float h) {
		const bool tall = h >= 2.0f * ValueBox::kHeight + kRowGap;
		for (size_t p = 0; p < pt::kPatterns; ++p) {
			PatternWidgets& pw = patterns_[p];
			const bool shown = open && p == focus_ && live(pt::at(p, Field::Next));
			pw.next->setBounds(x, top, w, ValueBox::kHeight);
			pw.repeats->setCompact(!tall);
			pw.repeats->setBounds(x, top + ValueBox::kHeight + kRowGap, w, tall ? ValueBox::kHeight : ValueBox::kCompactHeight);
			pw.next->setVisible(shown);
			pw.repeats->setVisible(shown && live(pt::at(p, Field::Repeats)));
		}
	};
	if (compact) {
		selector_->setBounds(left, y, kw, controlsHeight());
		selector_->setVisible(open && !cycling);
		seatCycle(left, y, kw, controlsHeight());
		const float bx = left + kw + kRowGap;
		const float bw = std::max(0.0f, inner - kw - kRowGap);
		keys_->setBounds(bx, y, bw, ValueBox::kHeight);
		const float chipW = std::min(64.0f, std::max(0.0f, (bw - kRowGap) * 0.5f));
		latch_->setBounds(bx, y + ValueBox::kHeight + kRowGap, chipW, SwitchChip::kHeight);
		cycle_->setBounds(bx + chipW + kRowGap, y + ValueBox::kHeight + kRowGap, chipW, SwitchChip::kHeight);
		keys_->setVisible(open);
		latch_->setVisible(open);
		cycle_->setVisible(open);
		humanize_->setVisible(false);
		wobble_->setVisible(false); // Humanize's neighbour: the window's, as Humanize is in the unit
		y += controlsHeight() + kGap;
	}
	else {
		// THE DEVICE'S OWN, above the tabs: they are no pattern's.
		const float latchWidth = std::min(52.0f, inner * 0.22f);
		const float boxes = std::max(0.0f, inner - 2.0f * latchWidth - 2.0f * kGap);
		keys_->setBounds(left, y, boxes, ValueBox::kHeight);
		const float chipY = y + 0.5f * (ValueBox::kHeight - SwitchChip::kHeight);
		cycle_->setBounds(left + inner - 2.0f * latchWidth - kGap, chipY, latchWidth, SwitchChip::kHeight);
		latch_->setBounds(left + inner - latchWidth, chipY, latchWidth, SwitchChip::kHeight);
		for (visage::Frame* f : std::initializer_list<visage::Frame*> { keys_.get(), latch_.get(), cycle_.get() }) {
			f->setVisible(open);
		}
		y += ValueBox::kHeight + kGap;
		selector_->setBounds(left, y, kw, controlsHeight());
		selector_->setVisible(open && !cycling);
		seatCycle(left, y, kw, controlsHeight());
		// HUMANIZE, AND WOBBLE AT ITS RIGHT: a small two-line box, centred on the cell's height.
		const float cellX = left + kw + kRowGap;
		const float cellW = std::max(0.0f, inner - kw - kRowGap);
		const float wobbleW = std::min(kWobbleWidth, std::max(0.0f, cellW * 0.4f));
		humanize_->setBounds(cellX, y, std::max(0.0f, cellW - wobbleW - kRowGap), controlsHeight());
		humanize_->setVisible(open);
		wobble_->setBounds(cellX + cellW - wobbleW, y + 0.5f * (controlsHeight() - ValueBox::kHeight), wobbleW, ValueBox::kHeight);
		wobble_->setVisible(open);
		(void)humanize_->refresh();
		y += controlsHeight() + kGap;
	}

	// THE TABS: the numbers (focus), and each one's E and S chips at its right.
	tabs_->setBounds(left, y, inner, kTabsHeight);
	tabs_->setVisible(open);
	const float cell = inner / static_cast<float>(pt::kPatterns);
	const float chip = std::max(0.0f, (cell - kTabsHeight - 3.0f * kRowGap) / 2.0f);
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		const float x = left + cell * static_cast<float>(p) + kTabsHeight;
		const float cy = y + 0.5f * (kTabsHeight - SwitchChip::kHeight);
		patterns_[p].enable->setBounds(x + kRowGap, cy, chip, SwitchChip::kHeight);
		patterns_[p].solo->setBounds(x + 2.0f * kRowGap + chip, cy, chip, SwitchChip::kHeight);
		patterns_[p].enable->setVisible(open);
		patterns_[p].solo->setVisible(open);
	}
	y += kTabsHeight;

	// THE FOCUSED PATTERN'S PANEL, joined to its tab: everything in it is that pattern's, and only that pattern's.
	const float panelTop = y;
	const float pad = kGap;
	const float pl = left + pad;
	const float pw = std::max(0.0f, inner - 2.0f * pad);
	y += kGap;
	const float laneH = compact ? kLaneHeightCompact : kLaneHeight;
	lane_->setBounds(pl, y, pw, laneH);
	lane_->setVisible(open);
	y += laneH + kGap;

	// THE FOCUSED PATTERN'S OWN: shown; the other three's are not drawn at all. COMPACT, the knobs take a quarter each and
	// Mode the half that is left as a short box beside them; Scale has a short row of its own under them, because the
	// longest scale names need the panel's width (the lead, 2026-11-05: "Melod", "Majo").
	const float pkw = std::max(0.0f, compact ? (pw - 2.0f * kRowGap) * 0.25f : (pw - 2.0f * kRowGap) / 3.0f);
	const float scaleTop = y + controlsHeight() + kGap;
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		PatternWidgets& w = patterns_[p];
		const bool shown = open && p == focus_;
		w.mode->setCompact(compact);
		w.scale->setCompact(compact);
		// COMPACT, THE WORDS ARE CUT TO WHAT FITS (the lead, 2026-11-05: "LEN…"): the Mode box prints its value alone
		// ("Melody Mono" says which box it is), and the Length knob is captioned "Len".
		if (w.mode->label().empty() != compact) {
			w.mode->setLabel(compact ? "" : "Mode");
			w.length->setLabel(compact ? "Len" : "Length");
		}
		if (compact) {
			const float bx = pl + 2.0f * (pkw + kRowGap);
			const float bw = std::max(0.0f, pl + pw - bx);
			w.mode->setBounds(bx, y + 0.5f * (controlsHeight() - ValueBox::kCompactHeight), bw, ValueBox::kCompactHeight);
			w.scale->setBounds(pl, scaleTop, pw, ValueBox::kCompactHeight);
		}
		else {
			w.mode->setBounds(pl, y, pw * 0.5f - 0.5f * kGap, ValueBox::kHeight);
			w.scale->setBounds(pl + pw * 0.5f + 0.5f * kGap, y, pw * 0.5f - 0.5f * kGap, ValueBox::kHeight);
		}
		w.mode->setVisible(shown);
		w.scale->setVisible(shown && live(pt::at(p, Field::Scale)));
	}
	if (!compact) {
		y += ValueBox::kHeight + kGap;
	}
	patterns_[focus_].rate->setBounds(pl, y, pkw, controlsHeight());
	patterns_[focus_].length->setBounds(pl + pkw + kRowGap, y, pkw, controlsHeight());
	patterns_[focus_].rate->setVisible(open);
	patterns_[focus_].length->setVisible(open && live(pt::at(focus_, Field::Length))); // a Progression runs its own
	y += controlsHeight() + kGap;
	if (compact) {
		y += ValueBox::kCompactHeight + kGap;
	}
	panel_ = { left, panelTop, inner, y - panelTop };
}

bool PatternCard::live(size_t index) const {
	const device::ParamDesc& d = pt::kParamDescs[index];
	if (index >= pt::kGlobalCount && index < pt::kParamCount) {
		const size_t p = (index - pt::kGlobalCount) / pt::kFieldCount;
		const size_t mode = pt::at(p, Field::Mode);
		const float m = mode < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, mode) : 0.0f;
		if (pt::drumHides(index, static_cast<pt::Mode>(std::clamp<long>(std::lround(m), 0, static_cast<long>(pt::Mode::kCount) - 1)))) {
			return false; // DRUM: its keys only gate, so no pitch box is drawn
		}
	}
	if (d.gateControl < 0) {
		return true;
	}
	const auto g = static_cast<size_t>(d.gateControl);
	const float v = g < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, g) : static_cast<float>(pt::kParamDescs[g].def);
	const auto bit = static_cast<unsigned>(std::lround(std::max(0.0f, v)));
	return bit < 32 && ((d.gateMask >> bit) & 1u) != 0u;
}

bool PatternCard::updateContent() {
	(void)foldRotateIfTurned(host_, device_);
	(void)clearStaleEdits(host_, device_);
	if (folded()) {
		return false;
	}
	// THE PLAYING DOT: the pattern the selector picks from the row (the device switches at its next step).
	std::vector<float> row(pt::kParamCount);
	const size_t present = host_.fieldsPresentAt(device_);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = i < present ? host_.modulatedFieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	}
	const pt::Settings now = pt::settingsOf(row.data(), row.size());
	showTabOutputs(); // a Mode change moves which lanes the card shows
	if (now.playMode == pt::PlayMode::Cycle) {
		// ON CYCLE THE ROW DOES NOT SAY WHICH PLAYS — the song position does: the device's playhead (none while idle).
		uint32_t ordinal = mod::Playhead::kIdle;
		float phase = 0.0f;
		const bool have = host_.playheadAt(device_, ordinal, phase) && ordinal != mod::Playhead::kIdle;
		tabs_->setPlaying(have ? std::min<size_t>(ordinal >> pt::PatternDevice::kPlayheadStepBits, pt::kPatterns) : pt::kPatterns);
	}
	else {
		tabs_->setPlaying(pt::selected(now));
	}
	// PLAY MODE MOVED: the selector or the cycle cell, whichever is read now.
	const size_t playMode = pt::at(Global::PlayMode);
	const float playModeNow = playMode < present ? host_.fieldAt(device_, playMode) : 0.0f;
	if (playModeNow != laidPlayMode_) {
		laidPlayMode_ = playModeNow;
		layoutControls();
	}
	// A GATE MAY HAVE MOVED (Humanize to Straight): its control's visibility follows, as the window's does.
	const bool humanizeMoved = humanize_->refresh();
	// THE FOCUSED MODE MAY HAVE MOVED (into Drum, out of it): what the card draws follows.
	const float modeNow = host_.fieldAt(device_, pt::at(focus_, Field::Mode));
	if (modeNow != laidMode_) {
		laidMode_ = modeNow;
		layoutControls();
	}
	return lane_->poll() || humanizeMoved;
}

void PatternCard::drawContent(visage::Canvas& canvas) {
	if (!folded() && panel_.height() > 0.0f) {
		PatternTabs::drawPanel(canvas, panel_.x(), panel_.y(), panel_.width(), panel_.height());
	}
}

bool PatternCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < pt::kDescriptor.outputs.size(); ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool PatternCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
