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

#include <visage/ui.h>

#include <string>
#include <vector>

// **◀ NAME ▶ AND ITS LIST — ONE WIDGET FOR EVERY "STEP THROUGH A LIST OF THINGS" CONTROL** (backlog FO:
// one widget so the browsers stay in sync). The preset browser is one `ItemSource`;
// Featherverb's IR file browser is to be the second. The widget knows nothing about what an item IS:
// it asks the source how many there are, what each is called, which is current, and tells it which to
// select. Everything a kind of item needs beyond that — saving presets, naming them, tags — is the
// source's, and reaches the list only through `actions()`.
//
// WHAT IT DOES, and every behaviour is the source's to switch:
//   - ◀ / ▶ step to the previous / next item, PASSING OVER any item the source reports as not
//     `loadable` (an unreadable file), and going round the ends only if the source `wraps`.
//   - A press on the name opens the LIST: every item, the current one marked; grouped into submenus by
//     `sections` (a folder, or each of an item's tags — an item may be in several); then a separator
//     and the source's `actions` — each at the foot or in a submenu of its own (`ItemAction::submenu`),
//     grouped by separators (`breakBefore`).
//   - A read-only item (`readOnly`) is named in a dimmer ink on the face. What read-only MEANS (cannot
//     be overwritten, deleted) is the source's.
//
// THE MENU'S ROW IDS: item `i` is `i`; action `a` is `kActionBase + a.id`. `choose(id)` is the one road a
// menu row takes, public so a case can take it too (a visage menu cannot be clicked headlessly).
//
// **A LOAD THAT FAILS IS SKIPPED LIKE ONE KNOWN NOT TO LOAD** (feathers-39, for the IR folder, where
// decoding every file up front costs too much): `selectItem` returns false when the load failed, and
// `step` then carries on to the next loadable item the same way — still at most one lap. The stepper
// REMEMBERS the failure: the list shows that row dimmed and unpickable and steps pass over it, until the
// source says its list changed (`forgetFailures`) or the count moves.
//
// **CALL FREQUENCY — THE SOURCE'S CONTRACT: every call below must be cheap; cache in the source.**
//   - EVERY PAINT of the face — which in a full-redraw build (the shipped one) is EVERY FRAME:
//     `itemCount`, `currentItem`, `itemLabel(current)`, `itemReadOnly(current)`, and `itemLoadable` for
//     as many items as it takes to find the next loadable one each way (to dim an arrow with nowhere to
//     go) — usually one each, at most a lap.
//   - ONLY WHEN THE LIST OPENS (`showList`, `listRows`): `itemLabel` and `itemSections` for every item,
//     and `itemActions`.
//   - ONLY ON A HAND'S GESTURE: `selectItem` (a step or a pick) and `itemAct`.
// So a source that lists a folder should read the folder when ITS path changes or when the list opens,
// and answer the per-paint calls from what it read — never touch the disk from them.

namespace feathers::ui {

/// One verb a source adds to the foot of the list.
///
/// **A SECOND LEVEL, FOR A SOURCE WHOSE VERBS WOULD CLOG THE LIST** (the menu was clogged with things
/// besides presets). `submenu` files the verb under a submenu of that name at the
/// foot — the submenu sits where its FIRST verb falls in `itemActions()`'s order — and `breakBefore`
/// draws a separator above it inside whichever menu it is in, so verbs can be grouped. Both empty/false
/// is a verb at the foot of the list with nothing between it and the one before, which is every action
/// written before these fields existed (Featherverb's IR browser keeps exactly that).
struct ItemAction {
	int id = 0; ///< the source's own number for it, handed back to `act`
	std::string label;
	bool enabled = true;
	std::string submenu {};   ///< empty: at the foot of the list itself
	bool breakBefore = false; ///< a separator above it, within its own menu
};

class ItemSource {
public:
	virtual ~ItemSource() = default;
	virtual size_t itemCount() const = 0;
	virtual std::string itemLabel(size_t index) const = 0;
	/// The current item, or -1 when none is.
	virtual int currentItem() const = 0;
	/// Make `index` current — load it, apply it, whatever an item is. Only ever called in range. FALSE:
	/// the load failed (and `index` should not have become current); a step then moves on past it.
	virtual bool selectItem(size_t index) = 0;
	/// A CHEAP HINT — false: stepping passes over it without trying (known not to load). Still listed.
	/// A failure only discovered by loading is `selectItem` returning false.
	virtual bool itemLoadable(size_t) const { return true; }
	virtual bool itemReadOnly(size_t) const { return false; }
	/// **LISTED AT ALL** — false keeps an item out of the list and out of ◀ ▶, while it stays selectable by index (the
	/// presets' variant files, reached only through their chips). True by default.
	virtual bool itemListed(size_t) const { return true; }
	/// Whether ◀ at the first item goes to the last, and ▶ at the last to the first.
	virtual bool itemsWrap() const { return false; }
	/// The sections the list files `index` under. Empty: at the top level.
	virtual std::vector<std::string> itemSections(size_t) const { return {}; }
	/// **SECTIONS INSIDE SECTIONS** — each a path of submenu names, outermost first (the preset browser's
	/// tag view: namespace ▸ value). By default each of `itemSections` as a path of one.
	virtual std::vector<std::vector<std::string>> itemSectionPaths(size_t index) const {
		std::vector<std::vector<std::string>> out;
		for (std::string& s : itemSections(index)) {
			out.push_back({ std::move(s) });
		}
		return out;
	}
	/// Where the submenu at `path` sorts among its siblings: lower first, ties by name. 0 for all by
	/// default, which is alphabetical — the order sections have always had.
	virtual double sectionOrder(const std::vector<std::string>&) const { return 0.0; }
	/// Told once at the start of every listing (`listRows`, so `showList` too), before any per-item call:
	/// a source whose sections need numbers over the whole list computes them here, once.
	virtual void itemsWillList() const {}
	/// The source's verbs, under the list.
	virtual std::vector<ItemAction> itemActions() const { return {}; }
	virtual void itemAct(int) {}
	/// What the face says when nothing is current.
	virtual std::string itemPlaceholder() const { return "-"; }
	/// A LINE AT THE HEAD OF THE LIST, dimmed and unpickable — what the list is not showing and why (the
	/// preset browser's tag view over a bank with no tags). Empty: no line.
	virtual std::string itemNote() const { return {}; }
};

class ItemStepper : public visage::Frame {
public:
	/// THE SIZE OF THE THREE PIECES — the only thing that differs between the places a stepper sits.
	struct Style {
		float height;
		float nameWidth;
		float arrowWidth;
	};

	ItemStepper(ItemSource& source, Style style);

	void draw(visage::Canvas& canvas) override;

	[[nodiscard]] float totalWidth() const noexcept { return style_.arrowWidth * 2.0f + style_.nameWidth; }
	[[nodiscard]] const Style& style() const noexcept { return style_; }

	bool hitsPrev(visage::Point position) const;
	bool hitsNext(visage::Point position) const;
	bool hitsName(visage::Point position) const;
	visage::Bounds nameBounds() const;

	/// ◀ (-1) / ▶ (+1): the next loadable item that way, round the ends if the source wraps; if its load
	/// FAILS, the one after, and so on — at most one lap. A no-op with nothing to go to, and while
	/// `setInteractive(false)`.
	void step(int direction);
	/// The index `step(direction)` would TRY first, or -1 — askable without selecting. Passes over items
	/// the source says will not load and items that have already failed.
	[[nodiscard]] int stepTarget(int direction) const;

	/// Open the list (a visage popup under the name box).
	void showList();
	/// **THE MENU `showList` SHOWS, BUILT BUT NOT SHOWN** — its submenus, separators and rows, for a case
	/// asking where a verb landed: `listRows` says which submenu a row NAMES, this is where it WENT.
	[[nodiscard]] visage::PopupMenu buildList() const;
	/// A list row's id, as the menu hands it back: an item or `kActionBase + action`.
	void choose(int id);
	static constexpr int kActionBase = 1 << 24;
	/// The source's `itemNote` row: disabled, and `choose` does nothing with it.
	static constexpr int kNoteId = -1;

	/// ONE ROW OF THE LIST AS IT WILL BE SHOWN — for the cases, which cannot read a popup.
	struct Row {
		/// Empty: top level. An item's section, or an action's `submenu` — tell the two apart by `id`
		/// (`kActionBase` and up is an action): a folder and a submenu may share a name and are still two.
		std::string section;
		int id = 0;          ///< what `choose` takes
		std::string label;
		bool current = false;
		bool enabled = true; ///< false for an action the source disabled, or an item that failed to load
		bool breakBefore = false; ///< an action's separator above it, within its own menu
		/// An item's submenus INSIDE `section`, outermost first; empty for one directly in it.
		std::vector<std::string> subsections {};
	};
	[[nodiscard]] std::vector<Row> listRows() const;

	/// Whether item `index` has failed to load since the last `forgetFailures`.
	[[nodiscard]] bool failed(size_t index) const;
	/// THE SOURCE'S LIST CHANGED (a new folder, a file replaced): every remembered failure is dropped.
	/// Also done by itself whenever `itemCount` moves.
	void forgetFailures() { failed_.clear(); }

	/// Off while something else owns the face (a source's prompt): steps and the list do nothing.
	void setInteractive(bool interactive) { interactive_ = interactive; }
	[[nodiscard]] bool interactive() const noexcept { return interactive_; }

protected:
	ItemSource& source() const noexcept { return source_; }
	/// Draw only the frame of the name box and the arrows — for a source drawing its own face over it.
	void drawChrome(visage::Canvas& canvas, uint32_t edge, bool arrowsLive);

private:
	visage::Bounds prevBounds() const;
	visage::Bounds nextBounds() const;

	/// Select `index` through the source; a false answer is remembered as a failure.
	bool trySelect(size_t index);
	/// The next candidate after `from` going `direction`, never reaching `stopAt` (the item the step
	/// started on), or -1.
	[[nodiscard]] int nextFrom(int from, int direction, int stopAt) const;
	/// Drop the failures if the count moved since they were recorded.
	void checkCount() const;

	ItemSource& source_;
	Style style_;
	bool interactive_ = true;
	/// Indices whose load failed, against the count they were recorded at.
	mutable std::vector<bool> failed_;
	/// kStyle's name box is clipped to leave room so the text under the arrows does not collide.
	static constexpr float kNamePadX = 12.0f;
};

} // namespace feathers::ui
