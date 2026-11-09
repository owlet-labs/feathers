/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/type_in.h"

#include "device/format_value.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/text_field.h"

#include <cmath>

namespace feathers::ui {

EditHistory* historyOf(const device::Modulatable& control) {
	const auto* records = dynamic_cast<const RecordsEdits*>(&control);
	return records != nullptr ? records->recordsInto() : nullptr;
}

TypeIn::TypeIn(visage::Frame& owner) : owner_(owner) {}

TypeIn::~TypeIn() {
	// THE FIELD CALLS NOBODY ON THE WAY OUT, `RouteList::~RouteList`'s reason: the owner is what is dying.
	if (field_ != nullptr) {
		field_->onCommit(nullptr);
		field_->onCancel(nullptr);
	}
}

void TypeIn::beginReset(device::Modulatable& control, bool amount, bool typeable) {
	closeIfOpen();
	control_ = &control;
	amount_ = amount;
	history_ = historyOf(control);
	before_ = current();
	// WHAT THE FIELD WILL SAY, ASKED NOW — after the reset the control reads its default.
	opening_ = !typeable ? std::nullopt
	           : amount  ? std::optional<std::string>(device::printAmountPercent(static_cast<float>(before_)))
	           : control.editText();
	serialBefore_ = history_ != nullptr ? history_->lastSerial() : 0;
	resetSerial_ = 0;
	armed_ = true;
}

void TypeIn::endReset() {
	if (!armed_ || history_ == nullptr) {
		return;
	}
	// A RESET THAT CHANGED NOTHING RECORDED NOTHING: the tip is where it was, and there is nothing to fold.
	const uint64_t now = history_->lastSerial();
	resetSerial_ = now != serialBefore_ ? now : 0;
}

bool TypeIn::open(visage::Bounds bounds) {
	if (!armed_ || control_ == nullptr || !opening_ || !owner_.isVisible()) {
		return false;
	}
	armed_ = false;
	if (field_ == nullptr) {
		field_ = std::make_unique<TextField>();
		// **HIDDEN FROM ITS OWN CALLBACKS, NEVER DESTROYED** — `text_field.h`. Hiding first, then acting:
		// the edit is already over, so the hide finds nothing to cancel.
		field_->onCommit([this](std::string text) {
			field_->setVisible(false);
			commit(text);
		});
		field_->onCancel([this] {
			field_->setVisible(false);
			cancel();
		});
		field_->setVisible(false);
		owner_.addChild(field_.get());
		// A SCALE CHANGE MOVES NOTHING LOGICAL (CLAUDE.md): re-set, so the native size follows the scale.
		field_->onDpiChange() += [this] { field_->setBounds(bounds_); };
	}
	place(bounds);
	open_ = true;
	field_->setVisible(true);
	field_->begin(*opening_);
	return true;
}

void TypeIn::place(visage::Bounds bounds) {
	bounds_ = bounds;
	if (field_ != nullptr) {
		field_->setBounds(bounds);
	}
}

bool TypeIn::closeIfOpen() {
	if (field_ == nullptr || !field_->editing()) {
		return false;
	}
	// HIDING IS THE CANCEL (`TextField::visibilityChanged`), and the cancel callback restores.
	field_->setVisible(false);
	return true;
}

bool TypeIn::editing() const noexcept { return field_ != nullptr && field_->editing(); }

double TypeIn::current() const {
	if (control_ == nullptr) {
		return 0.0;
	}
	return amount_ ? static_cast<double>(control_->display().amount) : control_->value();
}

void TypeIn::commit(const std::string& text) {
	if (!open_ || control_ == nullptr) {
		return;
	}
	// AN UNCHANGED ENTER IS NOT AN EDIT: the field opened on a text that reads back to the value, but the
	// value is what the hand left, to the last bit, and putting it back exactly is a cancel's job.
	if (opening_ && text == *opening_) {
		cancel();
		return;
	}
	std::optional<double> typed;
	if (amount_) {
		if (const std::optional<float> a = device::parseAmountPercent(text, device::kMaxAmount)) {
			// THE DRAG'S RELEASE RULE: an amount at nothing is gone, not a route at 0 %.
			typed = std::abs(*a) < device::kAmountUnroutedBelow ? 0.0 : static_cast<double>(*a);
		}
	}
	else {
		typed = control_->parseText(rewrite_ ? rewrite_(text) : text, before_);
	}
	// NOT A NUMBER IS NO VALUE — nothing moves, as for an Escape (the route list's precedent).
	if (!typed || *typed == before_) {
		cancel();
		return;
	}
	// A VALUE THAT LANDED BACK ON WHERE THE HAND STARTED (the control rounded it there) folded into a step
	// that does nothing: take the whole of it back rather than leave it on the stack.
	if (write(*typed) && current() == before_) {
		(void)history_->retractLast(history_->lastSerial());
	}
	forget();
	owner_.redraw();
}

bool TypeIn::write(double after) {
	const bool fold = history_ != nullptr && resetSerial_ != 0 && history_->lastSerial() == resetSerial_;
	if (fold) {
		history_->beginGroup("Type", resetSerial_);
	}
	if (amount_) {
		control_->beginAmountGesture();
		control_->setAmount(static_cast<float>(after));
		control_->endAmountGesture();
	}
	else {
		control_->beginGesture();
		control_->setValue(after);
		control_->endGesture();
	}
	if (fold) {
		history_->endGroup();
	}
	return fold;
}

void TypeIn::cancel() {
	if (!open_ || control_ == nullptr) {
		forget();
		return;
	}
	// THE RESET'S STEP TAKEN BACK, if it is still the tip — undone and gone, so the stack is as it was.
	if (history_ != nullptr && resetSerial_ != 0) {
		(void)history_->retractLast(resetSerial_);
	}
	// AND WHATEVER IS STILL NOT WHERE IT WAS — a control that records no steps (a host parameter), or a
	// reset whose step something else has since buried — goes back through its own gesture.
	if (current() != before_) {
		resetSerial_ = 0;
		write(before_);
	}
	forget();
	owner_.redraw();
}

void TypeIn::forget() noexcept {
	open_ = false;
	armed_ = false;
	control_ = nullptr;
	history_ = nullptr;
	opening_.reset();
	resetSerial_ = 0;
	serialBefore_ = 0;
}

} // namespace feathers::ui
