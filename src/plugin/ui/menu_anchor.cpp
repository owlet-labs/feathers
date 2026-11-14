/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/menu_anchor.h"

#include "plugin/ui/escape_stack.h"
#include "plugin/ui/surface_layers.h"

#include <algorithm>
#include <memory>

namespace feathers::ui {

namespace {

/// THE MENU THAT IS (OR WAS LAST) OPEN — identities only, never dereferenced until each has been found
/// again among live frames. `closed` is shared with the menu's own callbacks, so a pick or a cancel
/// marks it without this file having to outlive anything.
struct Open {
	const visage::Frame* source = nullptr;
	visage::Bounds anchor;
	const visage::Frame* popup = nullptr;
	std::shared_ptr<bool> closed;
};

Open& openMenu() {
	static Open o;
	return o;
}

MenuPlacement& lastPlacement() {
	static MenuPlacement p;
	return p;
}

/// THE POPUP, IF IT IS STILL A CHILD OF `top` — the only way to know the pointer is live.
visage::PopupMenuFrame* livePopup(const visage::Frame* top, const visage::Frame* popup) {
	if (top == nullptr || popup == nullptr) {
		return nullptr;
	}
	for (visage::Frame* child : top->children()) {
		if (child == popup) {
			return dynamic_cast<visage::PopupMenuFrame*>(child);
		}
	}
	return nullptr;
}

/// `wanted`, IF IT IS STILL A FRAME UNDER `top` — found by identity among live frames, never followed.
visage::Frame* liveUnder(visage::Frame* top, const visage::Frame* wanted) {
	if (top == nullptr) {
		return nullptr;
	}
	if (top == wanted) {
		return top;
	}
	for (visage::Frame* child : top->children()) {
		if (visage::Frame* found = liveUnder(child, wanted); found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

/// AN OPEN MENU'S ENTRY ON THE ESC STACK, held by a child the popup owns so the two go together. Invisible
/// and deaf: it is a lifetime, not a widget.
class EscapeHolder final : public visage::Frame {
public:
	explicit EscapeHolder(EscapeTicket ticket) : ticket_(std::move(ticket)) { setIgnoresMouseEvents(true, false); }
	bool closeTop() { return ticket_.closeTop(); }

private:
	EscapeTicket ticket_;
};

bool sameBounds(const visage::Bounds& a, const visage::Bounds& b) {
	return a.x() == b.x() && a.y() == b.y() && a.width() == b.width() && a.height() == b.height();
}

/// OPEN BY THE TOGGLE'S DEFINITION — see the header: present, not picked or cancelled, and not yet
/// faded by a paint since it lost the focus.
visage::PopupMenuFrame* openPopupOf(const visage::Frame& source) {
	const Open& o = openMenu();
	if (o.source != &source || o.closed == nullptr || *o.closed) {
		return nullptr;
	}
	visage::PopupMenuFrame* popup = livePopup(const_cast<visage::Frame&>(source).topParentFrame(), o.popup);
	if (popup == nullptr || popup->opacity() < 1.0f) {
		return nullptr;
	}
	return popup;
}

} // namespace

bool menuOpenFor(const visage::Frame& source) {
	return openPopupOf(source) != nullptr;
}

bool showMenuAt(visage::PopupMenu menu, visage::Frame& source, const visage::Bounds& anchor) {
	// THE TOGGLE FIRST: the same box, pressed while its menu is open, closes it and opens nothing.
	visage::PopupMenuFrame* popup = openPopupOf(source);
	if (popup != nullptr && sameBounds(openMenu().anchor, anchor)) {
		*openMenu().closed = true;
		popup->exit();
		lastPlacement().shown = false;
		return false;
	}

	// **MARKED CLOSED BEFORE THE SITE'S OWN CALLBACKS RUN, NOT AFTER** — so a menu opened FROM a pick
	// ("Add before…" opening the add menu, a Delete row opening its confirmation) is a replacement and not
	// a re-click. visage delivers the pick while the picked-from popup is still attached and opaque, and it
	// is usually the same box asking, so marked after, the toggle above read the cascade as a second
	// press and closed both menus (a regression on 'add before' in fx lanes).
	const auto picked = menu.onSelection();
	const auto cancelled = menu.onCancel();
	auto closed = std::make_shared<bool>(false);
	visage::Frame* const top = source.topParentFrame();
	// **THE SITE'S CALLBACKS RUN ONLY IF THE BOX IS STILL A LIVE FRAME UNDER THE WINDOW**, found again by identity
	// (`liveUnder`) and never followed: the sites capture `this`, and a card rebuilt by a state or preset load
	// while its menu is open leaves a pick to run on a dead box. `top` is the popup's own parent, so it is
	// live whenever either callback is delivered. A pick that finds the box gone only marks the menu closed.
	visage::Frame* const box = &source;
	menu.onSelection() = [closed, picked, top, box](int id) {
		*closed = true;
		if (liveUnder(top, box) != nullptr) {
			picked.callback(id);
		}
	};
	menu.onCancel() = [closed, cancelled, top, box]() {
		*closed = true;
		if (liveUnder(top, box) != nullptr) {
			cancelled.callback();
		}
	};

	menu.show(&source, { anchor.x(), anchor.bottom() });

	// THE FRAME visage JUST ADDED — the last popup among the top frame's children.
	visage::PopupMenuFrame* shown = nullptr;
	const auto& children = top->children();
	for (auto it = children.rbegin(); it != children.rend() && shown == nullptr; ++it) {
		shown = dynamic_cast<visage::PopupMenuFrame*>(*it);
	}
	if (shown == nullptr || shown->children().empty()) {
		return true;
	}
	// ITS FIRST LIST IS THE MENU'S ROWS, at the size visage measured for them.
	visage::Frame* list = shown->children().front();
	const float w = list->width();
	const float h = list->height();
	const float windowW = shown->width();
	const float windowH = shown->height();

	const visage::Bounds at = top->relativeBounds(&source);
	const visage::Bounds a { at.x() + anchor.x(), at.y() + anchor.y(), anchor.width(), anchor.height() };

	// BELOW IF IT FITS, ABOVE IF THAT FITS — left edges together, or right edges together when the
	// window is too narrow for that.
	float x = a.x();
	float y = a.bottom();
	const bool fitsBelow = a.bottom() + h <= windowH;
	const bool fitsAbove = a.y() - h >= 0.0f;
	if (fitsBelow || fitsAbove) {
		y = fitsBelow ? a.bottom() : a.y() - h;
		if (x + w > windowW) {
			x = std::max(0.0f, std::min(a.right() - w, windowW - w));
		}
	}
	else {
		// **NEITHER — A SMALL WINDOW, WHICH A POPPED-OUT PANEL IS.** Over the box is the one place it
		// must not go, so it goes BESIDE it: right of the box if that fits, else left, as far down the
		// box's own line as the window allows.
		y = std::clamp(a.y(), 0.0f, std::max(0.0f, windowH - h));
		if (a.right() + w <= windowW) {
			x = a.right();
		}
		else if (a.x() - w >= 0.0f) {
			x = a.x() - w;
		}
		else {
			x = std::max(0.0f, windowW - w);
		}
	}
	// **AND WHEREVER THAT PUT IT, INSIDE THE WINDOW** (a context menu that went off the bottom was
	// clipped and could not be reached). The rules above measure room from the ANCHOR, and an
	// anchor is not always in the window: a box laid out past the window's foot fits "above" by that
	// arithmetic while the menu's own foot is still below the last row. Clamped, it sits on the window's
	// edge, as near the box as the window allows.
	x = std::clamp(x, 0.0f, std::max(0.0f, windowW - w));
	y = std::clamp(y, 0.0f, std::max(0.0f, windowH - h));
	list->setBounds(x, y, w, h);

	// **OPENED ON THE CURRENT CHOICE** (`menu_anchor.h` has the why). The first top-level
	// row flagged `select(true)` is the current one; a list taller than its room is scrolled so that row sits
	// in the middle of what is shown, clamped at the ends so a row near either end is shown with the end.
	int current = -1;
	for (size_t i = 0; i < menu.options().size(); ++i) {
		if (menu.options()[i].selected()) {
			current = static_cast<int>(i);
			break;
		}
	}
	float scroll = 0.0f;
	if (auto* rows = dynamic_cast<visage::PopupList*>(list); rows != nullptr && current >= 0) {
		const float full = rows->renderHeight();
		if (full > h + 0.5f) {
			const auto rowTop = static_cast<float>(rows->yForIndex(current));
			const float rowH = static_cast<float>(rows->yForIndex(current + 1)) - rowTop;
			rows->setScrollableHeight(full, h);
			rows->setYPosition(std::clamp(rowTop + 0.5f * rowH - 0.5f * h, 0.0f, full - h));
			scroll = rows->yPosition();
		}
	}

	openMenu() = { &source, anchor, shown, closed };
	lastPlacement() = { a, { x, y, w, h }, windowW, windowH, true, current, scroll };

	// **ESC CLOSES IT** (backlog NN) — it had no keyboard exit at all: visage's popup takes the keyboard
	// focus and has no key handler, and its parent is the window's top frame, above the editor root, so the
	// key walked up past nobody. On the editor's Esc stack while it is open, so the order is the one stack's;
	// a menu holds the focus and any press elsewhere closes it, so in practice it is the top.
	const auto escape = [shown, closed, cancelled, box] {
		if (*closed || shown->opacity() < 1.0f) {
			return false;
		}
		*closed = true;
		if (liveUnder(shown->parent(), box) != nullptr) { // the site's cancel only for a box still there
			cancelled.callback();
		}
		shown->exit();
		// THE KEYBOARD GOES BACK WHERE THE PRESS THAT OPENED THE MENU LEFT IT — the first frame up from the
		// box that takes keys — so the next Esc reaches the editor's stack rather than nobody. Only if the box
		// is still a live frame under the window, found again rather than followed.
		if (visage::Frame* live = liveUnder(shown->parent(), box); live != nullptr) {
			visage::Frame* keys = live;
			while (keys != nullptr && !keys->acceptsKeystrokes()) {
				keys = keys->parent();
			}
			if (keys != nullptr) {
				keys->requestKeyboardFocus();
			}
		}
		return true;
	};
	EscapeStack* stack = SurfaceLayers::escapeStackFor(source);
	if (stack != nullptr) {
		// THE ENTRY LIVES AS LONG AS THE POPUP: held by a child it owns, so the popup going by any road — a
		// pick, a click elsewhere fading it — takes the entry with it.
		shown->addChild(std::make_unique<EscapeHolder>(stack->push(escape)), false);
	}
	shown->onKeyPress() = [stack = stack != nullptr, shown, escape](const visage::KeyEvent& e) {
		if (e.keyCode() != visage::KeyCode::Escape) {
			return false;
		}
		if (stack) {
			for (visage::Frame* child : shown->children()) {
				if (auto* holder = dynamic_cast<EscapeHolder*>(child); holder != nullptr) {
					return holder->closeTop();
				}
			}
		}
		return escape();
	};
	return true;
}

void dismissMenuOf(visage::Frame& source) {
	// THE REMEMBERED SOURCE IS COMPARED, NEVER FOLLOWED — the one passed in is the live one.
	Open& o = openMenu();
	if (o.source != &source || o.closed == nullptr || *o.closed) {
		return;
	}
	*o.closed = true;
	if (visage::PopupMenuFrame* popup = livePopup(source.topParentFrame(), o.popup); popup != nullptr) {
		popup->exit();
	}
}

MenuPlacement lastMenuPlacementForTest() {
	return lastPlacement();
}

std::string markedChoiceLabel(const std::string& name, bool current) {
	return (current ? std::string("X  ") : std::string("     ")) + name;
}

visage::PopupMenu& addChoiceRow(visage::PopupMenu& menu, int id, const std::string& name, bool current) {
	return menu.addOption(id, markedChoiceLabel(name, current)).select(current);
}

} // namespace feathers::ui
