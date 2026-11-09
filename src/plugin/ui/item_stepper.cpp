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

#include "plugin/ui/item_stepper.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <map>
#include <memory>

namespace feathers::ui {

namespace {

/// The box border: a 1px frame, the engine selector's treatment, so every stepper reads as one kind.
constexpr float kBorder = 1.0f;

void drawPanel(visage::Canvas& canvas, const visage::Bounds& b, uint32_t edge) {
	canvas.setColor(edge);
	canvas.fill(b.x(), b.y(), b.width(), b.height());
	canvas.setColor(kSurfaceRaised);
	canvas.fill(b.x() + kBorder, b.y() + kBorder, b.width() - 2.0f * kBorder, b.height() - 2.0f * kBorder);
}

} // namespace

ItemStepper::ItemStepper(ItemSource& source, Style style) : source_(source), style_(style) {
	// ITS OWN MOUSE, IN ITS OWN COORDINATES — a host that translated clicks into it once aimed them at
	// a box it was not drawing (the header's history, `preset_browser.h`).
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (hitsPrev(e.position)) {
			step(-1);
		}
		else if (hitsNext(e.position)) {
			step(1);
		}
		else if (hitsName(e.position)) {
			showList();
		}
	};
}

visage::Bounds ItemStepper::prevBounds() const {
	const float y = (height() - style_.height) * 0.5f;
	return { 0.0f, y, style_.arrowWidth, style_.height };
}

visage::Bounds ItemStepper::nameBounds() const {
	const float y = (height() - style_.height) * 0.5f;
	return { style_.arrowWidth, y, style_.nameWidth, style_.height };
}

visage::Bounds ItemStepper::nextBounds() const {
	const float y = (height() - style_.height) * 0.5f;
	return { style_.arrowWidth + style_.nameWidth, y, style_.arrowWidth, style_.height };
}

bool ItemStepper::hitsPrev(visage::Point position) const {
	return prevBounds().contains(position);
}

bool ItemStepper::hitsNext(visage::Point position) const {
	return nextBounds().contains(position);
}

bool ItemStepper::hitsName(visage::Point position) const {
	return nameBounds().contains(position);
}

void ItemStepper::checkCount() const {
	if (!failed_.empty() && failed_.size() != source_.itemCount()) {
		failed_.clear(); // the list changed under the failures; they no longer name anything
	}
}

bool ItemStepper::failed(size_t index) const {
	checkCount();
	return index < failed_.size() && failed_[index];
}

bool ItemStepper::trySelect(size_t index) {
	if (source_.selectItem(index)) {
		return true;
	}
	checkCount();
	if (failed_.empty()) {
		failed_.assign(source_.itemCount(), false);
	}
	if (index < failed_.size()) {
		failed_[index] = true;
	}
	return false;
}

int ItemStepper::nextFrom(int from, int direction, int stopAt) const {
	const int n = static_cast<int>(source_.itemCount());
	if (direction == 0 || n == 0) {
		return -1;
	}
	const int dir = direction > 0 ? 1 : -1;
	// FROM NOTHING, ▶ starts at the first and ◀ at the last.
	int at = from < 0 || from >= n ? (dir > 0 ? -1 : n) : from;
	// AT MOST ONE LAP, so a list where nothing loads ends here rather than spinning.
	for (int tried = 0; tried < n; ++tried) {
		at += dir;
		if (at < 0 || at >= n) {
			if (!source_.itemsWrap()) {
				return -1; // past an end, and this source does not go round
			}
			at = (at + n) % n;
		}
		if (at == stopAt) {
			return -1; // came all the way round to where the step started
		}
		if (source_.itemListed(static_cast<size_t>(at)) && source_.itemLoadable(static_cast<size_t>(at)) && !failed(static_cast<size_t>(at))) {
			return at;
		}
	}
	return -1;
}

int ItemStepper::stepTarget(int direction) const {
	const int from = source_.currentItem();
	return nextFrom(from, direction, from);
}

void ItemStepper::step(int direction) {
	if (!interactive_) {
		return;
	}
	const int start = source_.currentItem();
	const int n = static_cast<int>(source_.itemCount());
	int from = start;
	// A LOAD THAT FAILS IS PASSED OVER LIKE ONE KNOWN NOT TO LOAD: try the next, still inside one lap.
	for (int tries = 0; tries < n; ++tries) {
		const int to = nextFrom(from, direction, start);
		if (to < 0) {
			break;
		}
		if (trySelect(static_cast<size_t>(to))) {
			break;
		}
		from = to;
	}
	redraw();
}

std::vector<ItemStepper::Row> ItemStepper::listRows() const {
	source_.itemsWillList();
	std::vector<Row> rows;
	const int current = source_.currentItem();
	const size_t n = source_.itemCount();
	if (std::string note = source_.itemNote(); !note.empty()) {
		rows.push_back({ {}, kNoteId, std::move(note), false, false });
	}
	// TOP LEVEL FIRST — items filed under no section — then each section, an item under every section
	// path it names.
	std::map<std::vector<std::string>, std::vector<size_t>> sectioned;
	for (size_t i = 0; i < n; ++i) {
		if (!source_.itemListed(i)) {
			continue; // NOT LISTED (`ItemSource::itemListed`): reached another way
		}
		const std::vector<std::vector<std::string>> paths = source_.itemSectionPaths(i);
		if (paths.empty()) {
			rows.push_back({ {}, static_cast<int>(i), source_.itemLabel(i), static_cast<int>(i) == current, !failed(i) });
		}
		for (const std::vector<std::string>& p : paths) {
			if (!p.empty()) {
				sectioned[p].push_back(i);
			}
		}
	}
	// IN THE SOURCE'S ORDER, LEVEL BY LEVEL: at the first level two paths part, the source's
	// `sectionOrder` of the two submenus there, then their names; a path before the paths inside it.
	std::vector<const std::vector<std::string>*> keys;
	keys.reserve(sectioned.size());
	for (const auto& [path, items] : sectioned) {
		keys.push_back(&path);
	}
	std::stable_sort(keys.begin(), keys.end(), [this](const std::vector<std::string>* a, const std::vector<std::string>* b) {
		for (size_t d = 0;; ++d) {
			if (d == a->size() || d == b->size()) {
				return a->size() < b->size();
			}
			if ((*a)[d] != (*b)[d]) {
				const double oa = source_.sectionOrder({ a->begin(), a->begin() + static_cast<std::ptrdiff_t>(d) + 1 });
				const double ob = source_.sectionOrder({ b->begin(), b->begin() + static_cast<std::ptrdiff_t>(d) + 1 });
				return oa != ob ? oa < ob : (*a)[d] < (*b)[d];
			}
		}
	});
	for (const std::vector<std::string>* path : keys) {
		for (const size_t i : sectioned[*path]) {
			rows.push_back({ path->front(), static_cast<int>(i), source_.itemLabel(i), static_cast<int>(i) == current,
			                 !failed(i), false, { path->begin() + 1, path->end() } });
		}
	}
	for (const ItemAction& a : source_.itemActions()) {
		rows.push_back({ a.submenu, kActionBase + a.id, a.label, false, a.enabled, a.breakBefore });
	}
	return rows;
}

void ItemStepper::choose(int id) {
	if (id >= kActionBase) {
		source_.itemAct(id - kActionBase);
		return;
	}
	if (id >= 0 && id < static_cast<int>(source_.itemCount())) {
		(void)trySelect(static_cast<size_t>(id));
		redraw();
	}
}

namespace {

/// A submenu of items being built: its rows, then its own submenus in the order they were first met.
struct SectionNode {
	std::string name;
	std::vector<const ItemStepper::Row*> items;
	std::vector<std::unique_ptr<SectionNode>> children;

	SectionNode* child(const std::string& n) {
		for (const auto& c : children) {
			if (c->name == n) {
				return c.get();
			}
		}
		children.push_back(std::make_unique<SectionNode>());
		children.back()->name = n;
		return children.back().get();
	}

	/// This node's rows, then each child as a submenu, into `menu`.
	void fill(visage::PopupMenu& menu) const {
		for (const ItemStepper::Row* row : items) {
			menu.addOption(row->id, row->label).select(row->current).enable(row->enabled);
		}
		for (const auto& c : children) {
			visage::PopupMenu sub(c->name);
			c->fill(sub);
			menu.addSubMenu(std::move(sub));
		}
	}
};

} // namespace

visage::PopupMenu ItemStepper::buildList() const {
	visage::PopupMenu menu;
	SectionNode sections;
	bool actionsBegun = false;
	// THE FOOT OF THE LIST, in `itemActions()` order: a verb, or an action submenu at the place its first
	// verb fell. Built first and added last, because visage takes a submenu BY VALUE — it has to be whole
	// before it goes in.
	struct Foot {
		int row = -1;        ///< a top-level verb: its index in `rows`
		std::string submenu; ///< else the submenu that goes here
		bool breakBefore = false;
	};
	std::vector<Foot> foot;
	std::map<std::string, visage::PopupMenu> actionSubs;
	const std::vector<Row> rows = listRows();
	for (size_t r = 0; r < rows.size(); ++r) {
		const Row& row = rows[r];
		if (row.id >= kActionBase) {
			if (!actionsBegun) {
				// THE SECTIONS GO IN BEFORE THE ACTIONS, so the list reads items, then verbs.
				sections.fill(menu);
				sections = SectionNode {};
				menu.addBreak();
				actionsBegun = true;
			}
			if (row.section.empty()) {
				foot.push_back({ static_cast<int>(r), {}, row.breakBefore });
				continue;
			}
			auto [it, fresh] = actionSubs.try_emplace(row.section, visage::PopupMenu(row.section));
			if (fresh) {
				// A SUBMENU'S FIRST VERB SAYING "A NEW GROUP" MEANS THE SUBMENU STARTS ONE, out in the list.
				foot.push_back({ -1, row.section, row.breakBefore });
			}
			else if (row.breakBefore) {
				it->second.addBreak();
			}
			it->second.addOption(row.id, row.label).enable(row.enabled);
			continue;
		}
		if (row.section.empty()) {
			menu.addOption(row.id, row.label).select(row.current).enable(row.enabled);
		}
		else {
			SectionNode* node = sections.child(row.section);
			for (const std::string& s : row.subsections) {
				node = node->child(s);
			}
			node->items.push_back(&row);
		}
	}
	sections.fill(menu);
	for (size_t f = 0; f < foot.size(); ++f) {
		if (foot[f].breakBefore && f > 0) {
			menu.addBreak(); // never straight under the break that already parts items from verbs
		}
		if (foot[f].row >= 0) {
			const Row& row = rows[static_cast<size_t>(foot[f].row)];
			menu.addOption(row.id, row.label).enable(row.enabled);
		}
		else {
			menu.addSubMenu(std::move(actionSubs[foot[f].submenu]));
		}
	}
	return menu;
}

void ItemStepper::showList() {
	if (!interactive_) {
		return;
	}
	visage::PopupMenu menu = buildList();
	menu.onSelection() = [this](int id) { choose(id); };
	const visage::Bounds name = nameBounds();
	showMenuAt(std::move(menu), *this, name);
}

void ItemStepper::drawChrome(visage::Canvas& canvas, uint32_t edge, bool arrowsLive) {
	const visage::Bounds prev = prevBounds();
	const visage::Bounds next = nextBounds();
	const visage::Font& control = font(kControlSize);
	drawPanel(canvas, nameBounds(), edge);
	canvas.setColor(arrowsLive && stepTarget(-1) >= 0 ? kMuted : kDisabled);
	canvas.text("\xe2\x97\x80", control, visage::Font::kCenter, prev.x(), prev.y(), prev.width(), prev.height());
	canvas.setColor(arrowsLive && stepTarget(1) >= 0 ? kMuted : kDisabled);
	canvas.text("\xe2\x96\xb6", control, visage::Font::kCenter, next.x(), next.y(), next.width(), next.height());
}

void ItemStepper::draw(visage::Canvas& canvas) {
	drawChrome(canvas, kLine, interactive_);
	const visage::Bounds name = nameBounds();
	const int current = source_.currentItem();
	const bool none = current < 0 || current >= static_cast<int>(source_.itemCount());
	const std::string shown = none ? source_.itemPlaceholder() : source_.itemLabel(static_cast<size_t>(current));
	canvas.setColor(none || source_.itemReadOnly(static_cast<size_t>(std::max(current, 0))) ? kMuted : kInk);
	canvas.text(shown, font(kControlSize), visage::Font::kLeft, name.x() + kNamePadX, name.y(),
	            name.width() - 2.0f * kNamePadX, name.height());
}

} // namespace feathers::ui
