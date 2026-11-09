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

#include "plugin/ui/lane_selection.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <string>

namespace feathers::ui {

LaneSelection::LaneSelection(rack::FragmentSite& site, EditHistory& history, Notifier* notifier, Hooks hooks)
    : verbs_(site, history, notifier, hooks.order), hooks_(std::move(hooks)) {}

void LaneSelection::setEscapeStack(EscapeStack* stack) {
	ticket_.release();
	escape_ = stack;
	syncTicket();
}

void LaneSelection::after() {
	syncTicket();
	if (hooks_.changed) {
		hooks_.changed();
	}
}

void LaneSelection::syncTicket() {
	// ONE ENTRY WHILE TWO OR MORE ARE SELECTED (D1, NN's stack): Esc clears the selection and the anchor keeps the
	// keyboard. A selection of one is the old focus, which Esc never cleared.
	if (selection_.size() >= 2) {
		if (!ticket_.held() && escape_ != nullptr) {
			ticket_ = escape_->push([this] {
				const DeviceInstanceId anchor = selection_.anchor();
				selection_.clear();
				if (anchor != kNoInstance) {
					selection_.touch(anchor);
				}
				ticket_.release(); // a close releases its own entry
				if (hooks_.changed) {
					hooks_.changed();
				}
				return true;
			});
		}
	}
	else {
		ticket_.release();
	}
}

void LaneSelection::focusPress(DeviceInstanceId id) {
	const bool samePress = before_.has_value() && before_->id == id && before_->after == selection_;
	if (!samePress) {
		before_ = Before { id, selection_, {} };
	}
	collapseOnClick_ = kNoInstance;
	selection_.touch(id);
	before_->after = selection_;
	after();
}

void LaneSelection::cardPress(DeviceInstanceId id, CardSelection::Press kind) {
	// PUT BACK WHAT THE FOCUS ROAD REPLACED for this very press, then apply what the hand held.
	if (before_.has_value() && before_->id == id) {
		selection_ = before_->selection;
	}
	before_.reset();
	collapseOnClick_ = kNoInstance;
	if (kind == CardSelection::Press::Plain && selection_.size() > 1 && selection_.contains(id)) {
		collapseOnClick_ = id; // the block may be dragged; a click that does not drag collapses to this card
	}
	else {
		selection_.press(id, kind, hooks_.order());
	}
	after();
}

void LaneSelection::cardClick(DeviceInstanceId id) {
	if (collapseOnClick_ == id && id != kNoInstance) {
		selection_.press(id, CardSelection::Press::Plain, hooks_.order());
		after();
	}
	collapseOnClick_ = kNoInstance;
}

void LaneSelection::lassoBegin(visage::Point at, bool add) {
	lassoFrom_ = at;
	lassoTo_ = at;
	lassoAdd_ = add;
	lassoBase_ = selection_;
	before_.reset();
}

void LaneSelection::lassoMove(visage::Point at) {
	if (!lassoFrom_.has_value()) {
		return;
	}
	lassoTo_ = at;
	const std::optional<visage::Bounds> rect = lassoRect();
	std::vector<DeviceInstanceId> touched;
	for (const DeviceInstanceId id : hooks_.order()) {
		const std::optional<visage::Bounds> card = hooks_.cardBounds ? hooks_.cardBounds(id) : std::nullopt;
		if (card.has_value() && rect.has_value() && card->overlaps(*rect)) {
			touched.push_back(id);
		}
	}
	selection_ = lassoAdd_ ? lassoBase_ : CardSelection {};
	selection_.lasso(touched, true);
	after();
}

void LaneSelection::lassoEnd() {
	lassoFrom_.reset();
	if (hooks_.changed) {
		hooks_.changed();
	}
}

std::optional<visage::Bounds> LaneSelection::lassoRect() const {
	if (!lassoFrom_.has_value()) {
		return std::nullopt;
	}
	const float x = std::min(lassoFrom_->x, lassoTo_.x);
	const float y = std::min(lassoFrom_->y, lassoTo_.y);
	return visage::Bounds(x, y, std::max(1.0f, std::abs(lassoTo_.x - lassoFrom_->x)),
	                      std::max(1.0f, std::abs(lassoTo_.y - lassoFrom_->y)));
}

bool LaneSelection::key(const visage::KeyEvent& e) {
	const visage::KeyCode code = e.keyCode();
	if ((code == visage::KeyCode::Delete || code == visage::KeyCode::Backspace || code == visage::KeyCode::KPBackspace)
	    && selection_.size() > 1) {
		(void)verbs_.remove(selection_.inOrder(hooks_.order()));
		selection_.clear();
		after();
		return true;
	}
	const size_t before = selection_.size();
	const std::vector<DeviceInstanceId> was = selection_.ids();
	if (!verbs_.key(e, selection_)) {
		return false;
	}
	if (selection_.size() != before || selection_.ids() != was) {
		after();
	}
	else {
		prune();
	}
	return true;
}

std::vector<DeviceInstanceId> LaneSelection::menuIds(DeviceInstanceId pressed) const {
	if (selection_.contains(pressed)) {
		return selection_.inOrder(hooks_.order());
	}
	return pressed != kNoInstance ? std::vector<DeviceInstanceId> { pressed } : std::vector<DeviceInstanceId> {};
}

void LaneSelection::addVerbs(visage::PopupMenu& menu, int firstId, DeviceInstanceId pressed) const {
	verbs_.addTo(menu, firstId, menuIds(pressed));
}

bool LaneSelection::chooseVerb(int id, int firstId, DeviceInstanceId pressed) {
	const std::vector<DeviceInstanceId> ids = menuIds(pressed);
	FragmentVerbs::MenuHooks hooks;
	hooks.select = [this](std::vector<DeviceInstanceId> next) {
		selection_.set(next);
		prune();
		after();
	};
	const bool chose = verbs_.choose(id, firstId, ids, hooks);
	if (chose) {
		prune();
		after();
	}
	return chose;
}

void LaneSelection::select(std::span<const DeviceInstanceId> ids) {
	selection_.set(ids);
	after();
}

void LaneSelection::prune() {
	if (selection_.prune(hooks_.order())) {
		after();
	}
}

void LaneSelection::clear() {
	selection_.clear();
	after();
}

void SelectionOverlay::draw(visage::Canvas& canvas) {
	// THE LASSO ONCE, from whichever chain is running it (both are, over one surface: the rectangle is the same).
	std::optional<visage::Bounds> lasso = selection_.lassoRect();
	if (!lasso.has_value() && second_ != nullptr) {
		lasso = second_->lassoRect();
	}
	if (const std::optional<visage::Bounds> rect = lasso; rect.has_value()) {
		canvas.setColor(withAlpha(kAccent, 0.08f));
		canvas.rectangle(rect->x(), rect->y(), rect->width(), rect->height());
		canvas.setColor(withAlpha(kAccent, 0.9f));
		canvas.rectangleBorder(rect->x(), rect->y(), rect->width(), rect->height(), 1.0f);
	}
	// "N SELECTED · ESC CLEARS" — what the verbs will act on, said once, small, at the surface's foot (D1).
	size_t n = selection_.selection().size();
	if (n < 2 && second_ != nullptr) {
		n = second_->selection().size();
	}
	if (n < 2) {
		return;
	}
	const std::string text = std::to_string(n) + " selected - Esc clears"; // ASCII: measured byte-wise
	const visage::Font& face = font(11.0f);
	const float w = face.stringWidth(std::u32string(text.begin(), text.end())) + 16.0f;
	const float h = 18.0f;
	const float x = pillLeft_;
	const float y = height() - h - 4.0f;
	canvas.setColor(withAlpha(kNotifyGround, 0.92f));
	canvas.roundedRectangle(x, y, w, h, h * 0.5f);
	canvas.setColor(kAccent);
	canvas.roundedRectangleBorder(x, y, w, h, h * 0.5f, 1.0f);
	canvas.setColor(kInk);
	canvas.text(text, face, visage::Font::kCenter, x, y, w, h);
}

} // namespace feathers::ui
