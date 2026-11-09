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

#include "plugin/ui/fragment_verbs.h"

#include "plugin/ui/notify_tray.h"
#include "rack/rack_device.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace feathers::ui {

namespace {

//------------------------------------------------------------------------------------------------//
// THE UNDO RECORDS. Each holds the site and the weak half of its life token, and nothing of a widget //
// (edit_history.h's rule): a step over a chain that has gone — a Chain window's device removed — does //
// nothing rather than reach a freed bridge. None contains a remap: each direction is one fragment    //
// verb (device_edits.h's rule).                                                                      //
//------------------------------------------------------------------------------------------------//

class SiteRecord : public EditRecord {
public:
	explicit SiteRecord(rack::FragmentSite& site) : site_(site), life_(site.fragmentLife()) {}

protected:
	/// The site, or null when its bridge has gone.
	rack::FragmentSite* site() const { return life_.expired() ? nullptr : &site_; }

private:
	rack::FragmentSite& site_;
	std::weak_ptr<const void> life_;
};

/// A PASTE OR A DUPLICATE: undo takes the copies away; redo puts them back under the same ids, as they landed.
class PlacedEdit final : public SiteRecord {
public:
	PlacedEdit(rack::FragmentSite& site, rack::Fragment placed, const char* verb)
	    : SiteRecord(site), placed_(std::move(placed)), verb_(verb) {}
	void undo() const override {
		if (rack::FragmentSite* s = site()) {
			const std::vector<DeviceInstanceId> ids = placed_.ids();
			(void)rack::removeFragmentDevices(*s, ids);
		}
	}
	void redo() const override {
		if (rack::FragmentSite* s = site()) {
			(void)rack::placeFragment(*s, placed_, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
		}
	}
	const char* verb() const override { return verb_; }

private:
	rack::Fragment placed_;
	const char* verb_;
};

/// A REMOVE OR A CUT: undo puts every device back where it stood, under its own id, with every route, label and
/// host slot it had; redo removes them again.
class RemovedEdit final : public SiteRecord {
public:
	RemovedEdit(rack::FragmentSite& site, rack::Fragment removed, const char* verb)
	    : SiteRecord(site), removed_(std::move(removed)), verb_(verb) {}
	void undo() const override {
		if (rack::FragmentSite* s = site()) {
			(void)rack::placeFragment(*s, removed_, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
		}
	}
	void redo() const override {
		if (rack::FragmentSite* s = site()) {
			const std::vector<DeviceInstanceId> ids = removed_.ids();
			(void)rack::removeFragmentDevices(*s, ids);
		}
	}
	const char* verb() const override { return verb_; }

private:
	rack::Fragment removed_;
	const char* verb_;
};

/// A GROUP: undo ungroups to exactly what was there; redo groups again under the same container id.
class GroupEdit final : public SiteRecord {
public:
	GroupEdit(rack::FragmentSite& site, rack::GroupPlan plan)
	    : SiteRecord(site), plan_(std::move(plan)) {}
	void undo() const override {
		if (rack::FragmentSite* s = site()) {
			(void)rack::undoGroup(*s, plan_.containerId, plan_.before);
		}
	}
	void redo() const override {
		if (rack::FragmentSite* s = site()) {
			rack::GroupPlan again = plan_;
			(void)rack::applyGroup(*s, again);
		}
	}
	const char* verb() const override { return plan_.kind == rack::GroupKind::Rack ? "Group into Rack" : "Group into Chain"; }

private:
	rack::GroupPlan plan_;
};

/// A BLOCK MOVE: undo puts the chain back in its old order and gives back the routes the move deleted.
class BlockMoveEdit final : public SiteRecord {
public:
	BlockMoveEdit(rack::FragmentSite& site, std::vector<DeviceInstanceId> before, std::vector<DeviceInstanceId> after,
	              std::vector<backplane::Route> routes)
	    : SiteRecord(site), before_(std::move(before)), after_(std::move(after)), routes_(std::move(routes)) {}
	void undo() const override {
		if (rack::FragmentSite* s = site()) {
			(void)rack::restoreOrder(*s, before_, routes_);
		}
	}
	void redo() const override {
		if (rack::FragmentSite* s = site()) {
			(void)rack::restoreOrder(*s, after_, {});
		}
	}
	const char* verb() const override { return "Move"; }

private:
	std::vector<DeviceInstanceId> before_;
	std::vector<DeviceInstanceId> after_;
	std::vector<backplane::Route> routes_;
};

/// A MOVE INTO ANOTHER CHAIN (backlog NZ): undo takes the devices out of the chain they landed in and puts them back
/// where they stood, with every route they had there; redo moves them again, as they landed. Either chain gone, nothing.
class MovedAcrossEdit final : public EditRecord {
public:
	MovedAcrossEdit(rack::FragmentSite& from, rack::FragmentSite& to, rack::Fragment lifted, rack::Fragment landed)
	    : from_(from), to_(to), fromLife_(from.fragmentLife()), toLife_(to.fragmentLife()), lifted_(std::move(lifted)),
	      landed_(std::move(landed)) {}
	void undo() const override {
		if (fromLife_.expired() || toLife_.expired()) {
			return;
		}
		(void)rack::removeFragmentDevices(to_, landed_.ids());
		(void)rack::placeFragment(from_, lifted_, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
	}
	void redo() const override {
		if (fromLife_.expired() || toLife_.expired()) {
			return;
		}
		(void)rack::removeFragmentDevices(from_, lifted_.ids());
		(void)rack::placeFragment(to_, landed_, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
	}
	const char* verb() const override { return "Move"; }

private:
	rack::FragmentSite& from_;
	rack::FragmentSite& to_;
	std::weak_ptr<const void> fromLife_;
	std::weak_ptr<const void> toLife_;
	rack::Fragment lifted_;
	rack::Fragment landed_;
};

std::string devicesWord(size_t n) { return std::to_string(n) + (n == 1 ? " device" : " devices"); }
std::string routesWord(size_t n) { return std::to_string(n) + (n == 1 ? " route" : " routes"); }

} // namespace

void FragmentVerbs::say(std::string text) {
	lastSaid_ = std::move(text);
	if (notifier_ != nullptr) {
		notifier_->say(lastSaid_);
	}
}

void FragmentVerbs::copy(std::span<const DeviceInstanceId> ids) {
	const rack::Fragment f = rack::liftFragment(site_, ids);
	if (f.empty()) {
		return;
	}
	rack::setPasteboard(f);
	say("Copied " + devicesWord(f.size()));
}

std::vector<DeviceInstanceId> FragmentVerbs::cut(std::span<const DeviceInstanceId> ids) {
	const rack::Fragment f = rack::liftFragment(site_, ids);
	if (f.empty()) {
		return {};
	}
	rack::setPasteboard(f);
	if (rack::removeFragmentDevices(site_, f.ids()) > 0) {
		history_.record(std::make_unique<RemovedEdit>(site_, f, "Cut"));
		say("Cut " + devicesWord(f.size()));
	}
	return {};
}

std::vector<DeviceInstanceId> FragmentVerbs::paste(DeviceInstanceId after) {
	const std::optional<rack::Fragment> clip = rack::pasteboard();
	if (!clip.has_value() || clip->empty()) {
		return {};
	}
	const rack::ChainRecord& record = site_.fragmentRecord();
	size_t position = record.length(site_.fragmentChain());
	if (after != kNoInstance && record.holds(after) && record.chainOf(after) == site_.fragmentChain()) {
		position = record.positionOf(after) + 1;
	}
	const rack::PlaceResult placed = rack::placeFragment(site_, *clip, position, rack::PlaceIds::Fresh, false);
	if (placed.tooDeep) {
		say(rack::kNestingFullMessage);
		return {};
	}
	if (placed.placed.empty()) {
		say("Nothing pasted: this chain refused every device");
		return {};
	}
	history_.record(std::make_unique<PlacedEdit>(site_, rack::liftFragment(site_, placed.placed), "Paste"));
	std::string line = "Pasted " + devicesWord(placed.placed.size());
	if (placed.devicesRefused > 0) {
		line += "; " + devicesWord(placed.devicesRefused) + " refused";
	}
	if (const size_t lost = placed.routesDropped + placed.innerRoutesDropped; lost > 0) {
		line += "; " + routesWord(lost) + " from outside the copy not carried";
	}
	say(line);
	return placed.placed;
}

std::vector<DeviceInstanceId> FragmentVerbs::duplicate(std::span<const DeviceInstanceId> ids) {
	const rack::Fragment f = rack::liftFragment(site_, ids);
	if (f.empty()) {
		return {};
	}
	const rack::PlaceResult placed =
	    rack::placeFragment(site_, f, f.positions.back() + 1, rack::PlaceIds::Fresh, false);
	if (placed.placed.empty()) {
		return {};
	}
	history_.record(std::make_unique<PlacedEdit>(site_, rack::liftFragment(site_, placed.placed), "Duplicate"));
	std::string line = "Duplicated " + devicesWord(placed.placed.size());
	if (const size_t lost = placed.routesDropped + placed.innerRoutesDropped; lost > 0) {
		line += "; " + routesWord(lost) + " not carried";
	}
	say(line);
	return placed.placed;
}

void FragmentVerbs::recordAdded(std::span<const DeviceInstanceId> ids, const char* verb) {
	rack::Fragment f = rack::liftFragment(site_, ids);
	if (!f.empty()) {
		history_.record(std::make_unique<PlacedEdit>(site_, std::move(f), verb));
	}
}

size_t FragmentVerbs::remove(std::span<const DeviceInstanceId> ids) {
	const rack::Fragment f = rack::liftFragment(site_, ids);
	if (f.empty()) {
		return 0;
	}
	const size_t gone = rack::removeFragmentDevices(site_, f.ids());
	if (gone > 0) {
		history_.record(std::make_unique<RemovedEdit>(site_, f, "Remove"));
		say("Removed " + devicesWord(gone));
	}
	return gone;
}

std::vector<DeviceInstanceId> FragmentVerbs::group(std::span<const DeviceInstanceId> ids, rack::GroupKind kind) {
	if (const std::string why = rack::groupRefusal(site_, ids); !why.empty()) {
		say(why);
		return {};
	}
	std::optional<rack::GroupPlan> plan = rack::planGroup(site_, ids, kind);
	if (!plan.has_value()) {
		return {};
	}
	const DeviceInstanceId container = rack::applyGroup(site_, *plan);
	if (container == kNoInstance) {
		return {};
	}
	std::string line = "Grouped " + devicesWord(plan->moved.size()) + (kind == rack::GroupKind::Rack ? " into a Rack" : " into a Chain");
	// A ROUTE THE NEW BOUNDARY CANNOT CARRY IS SAID, never silently dropped (OA): the undo brings it back.
	if (plan->lost() > 0) {
		line += "; " + routesWord(plan->lost()) + " could not cross: " + plan->firstLost;
	}
	if (plan->polyOnBus > 0) {
		line += "; " + devicesWord(plan->polyOnBus) + " with Poly on now run once, on the bus";
	}
	if (plan->slotsLost > 0) {
		line += "; " + std::to_string(plan->slotsLost) + " host parameter(s) could not follow";
	}
	history_.record(std::make_unique<GroupEdit>(site_, *plan));
	say(line);
	return { container };
}

bool FragmentVerbs::moveBlock(std::span<const DeviceInstanceId> ids, size_t gap) {
	const std::vector<DeviceInstanceId> before = order_();
	const std::vector<backplane::Route> routes = rack::routesTouching(site_, ids);
	if (!rack::moveBlock(site_, ids, gap)) {
		return false;
	}
	history_.record(std::make_unique<BlockMoveEdit>(site_, before, order_(), routes));
	return true;
}

std::vector<DeviceInstanceId> FragmentVerbs::moveInto(rack::FragmentSite& to, std::span<const DeviceInstanceId> ids,
                                                      size_t position) {
	const rack::Fragment lifted = rack::liftFragment(site_, ids);
	if (lifted.empty()) {
		return {};
	}
	// THE DEPTH GATE, ASKED BEFORE ANYTHING LEAVES (`fitsNesting`): too tall for the chain it is dropped in, and
	// nothing moves.
	if (!rack::fragmentFits(to, lifted)) {
		say(rack::kNestingFullMessage);
		return {};
	}
	const std::vector<DeviceInstanceId> moved = lifted.ids();
	const std::vector<backplane::Route> touching = rack::routesTouching(site_, moved);
	(void)rack::removeFragmentDevices(site_, moved);
	const rack::PlaceResult placed = rack::placeFragment(to, lifted, position, rack::PlaceIds::Keep, false);
	if (placed.placed.empty() || placed.devicesRefused > 0) {
		// ALL OR NOTHING: what landed comes out again, and everything goes back where it stood, routes and all.
		(void)rack::removeFragmentDevices(to, placed.placed);
		(void)rack::placeFragment(site_, lifted, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
		say("Nothing moved: that chain refused " + devicesWord(std::max<size_t>(placed.devicesRefused, 1)));
		return {};
	}
	// THE HOST'S SOURCES ARE THE SAME ON BOTH SIDES: a route from outside the chain into a moved device names a source
	// both chains read through the same host, so it is kept wherever the new chain's gate admits it (a POST device moved
	// into PRE loses its route from PRE's own export, which PRE's gate refuses: it would read itself).
	size_t kept = 0;
	rack::ChainRecord& record = to.fragmentRecord();
	for (const backplane::Route& r : touching) {
		if (!r.source.isOuter() || !r.destination.isDevice()
		    || std::find(moved.begin(), moved.end(), r.destination.device) == moved.end()) {
			continue;
		}
		if (record.mayRoute(r.source, r.destination) && record.setRoute(r.source, r.destination, r.amount)) {
			(void)record.setRouteTransfer(r.source, r.destination, r.transfer);
			++kept;
		}
	}
	if (kept > 0) {
		const rack::DeviceLabels* labels = to.fragmentLabels();
		to.fragmentChanged(labels != nullptr ? *labels : rack::DeviceLabels {});
	}
	history_.record(std::make_unique<MovedAcrossEdit>(site_, to, lifted, rack::liftFragment(to, placed.placed)));
	const size_t lost = placed.routesDropped + placed.outboundLeft + placed.innerRoutesDropped - std::min(kept, placed.routesDropped);
	std::string line = "Moved " + devicesWord(placed.placed.size());
	if (lost > 0) {
		line += "; " + routesWord(lost) + " could not cross";
	}
	say(line);
	return placed.placed;
}

//------------------------------------------------------------------------------------------------//
// THE MENU AND THE KEYS                                                                            //
//------------------------------------------------------------------------------------------------//

void FragmentVerbs::addTo(visage::PopupMenu& menu, int firstId, std::span<const DeviceInstanceId> ids) const {
	const size_t n = ids.size();
	const std::string count = n > 1 ? " " + std::to_string(n) : std::string {};
	const std::string why = groupRefusal(ids);
	// NO GROUP ROWS WHERE NOTHING CAN BE GROUPED (the modulator strip, backlog PN): a row that never enables is clutter.
	if (site_.fragmentGroups()) {
		menu.addBreak();
		menu.addOption(firstId + static_cast<int>(FragmentVerb::GroupChain), "Group" + count + " into Chain").enable(why.empty());
		menu.addOption(firstId + static_cast<int>(FragmentVerb::GroupRack), "Group" + count + " into Rack").enable(why.empty());
	}
	menu.addBreak();
	menu.addOption(firstId + static_cast<int>(FragmentVerb::Copy), "Copy" + count);
	menu.addOption(firstId + static_cast<int>(FragmentVerb::Cut), "Cut" + count);
	menu.addOption(firstId + static_cast<int>(FragmentVerb::Paste), "Paste after").enable(canPaste());
	menu.addOption(firstId + static_cast<int>(FragmentVerb::Duplicate), "Duplicate" + count);
	if (n > 1) {
		menu.addOption(firstId + static_cast<int>(FragmentVerb::Remove), "Remove" + count);
	}
}

bool FragmentVerbs::choose(int id, int firstId, std::span<const DeviceInstanceId> ids, const MenuHooks& hooks) {
	const int verb = id - firstId;
	if (verb < 0 || verb >= static_cast<int>(FragmentVerb::Count)) {
		return false;
	}
	std::vector<DeviceInstanceId> next;
	bool select = true;
	switch (static_cast<FragmentVerb>(verb)) {
	case FragmentVerb::Copy:
		copy(ids);
		select = false;
		break;
	case FragmentVerb::Cut:
		next = cut(ids);
		break;
	case FragmentVerb::Paste:
		next = paste(ids.empty() ? kNoInstance : ids.back());
		break;
	case FragmentVerb::Duplicate:
		next = duplicate(ids);
		break;
	case FragmentVerb::GroupChain:
		next = group(ids, rack::GroupKind::Chain);
		break;
	case FragmentVerb::GroupRack:
		next = group(ids, rack::GroupKind::Rack);
		break;
	case FragmentVerb::Remove:
		(void)remove(ids);
		break;
	case FragmentVerb::Count:
		return false;
	}
	if (select && hooks.select) {
		hooks.select(std::move(next));
	}
	return true;
}

bool FragmentVerbs::key(const visage::KeyEvent& e, CardSelection& selection) {
	if (!e.isMainModifier() || e.isAltDown()) {
		return false;
	}
	const std::vector<DeviceInstanceId> chain = order_();
	const std::vector<DeviceInstanceId> ids = selection.inOrder(chain);
	switch (e.keyCode()) {
	case visage::KeyCode::A:
		selection.lasso(chain, false);
		return true;
	case visage::KeyCode::C:
		if (ids.empty()) {
			return false;
		}
		copy(ids);
		return true;
	case visage::KeyCode::X:
		if (ids.empty()) {
			return false;
		}
		(void)cut(ids);
		selection.clear();
		return true;
	case visage::KeyCode::V:
		if (!canPaste()) {
			return false;
		}
		selection.set(paste(ids.empty() ? kNoInstance : ids.back()));
		return true;
	case visage::KeyCode::D:
		if (ids.empty()) {
			return false;
		}
		selection.set(duplicate(ids));
		return true;
	case visage::KeyCode::G: {
		if (ids.empty() || !site_.fragmentGroups()) {
			return false;
		}
		const std::vector<DeviceInstanceId> made = group(ids, e.isShiftDown() ? rack::GroupKind::Rack : rack::GroupKind::Chain);
		if (!made.empty()) {
			selection.set(made);
		}
		return true;
	}
	default:
		return false;
	}
}

} // namespace feathers::ui
