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

#pragma once

#include "device/modulatable.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <visage/ui.h>

// **TYPE-IN ON A KNOB OR A VALUE BOX — backlog LS, decided 2026-10-06:** no double-click reset; a
// triple-click types.
//
// visage counts clicks itself (`windowing.cpp`), so a triple-click arrives as a 2 and then a 3, and the
// double-click's reset ALWAYS runs on the way. That fixes the shape of everything here:
//
// - the field opens on the value from BEFORE the reset, all of it selected — so the widget tells this
//   class about the reset as it makes it (`beginReset`/`endReset`) and the third click only opens;
// - Enter writes the typed value and FOLDS the reset into the same undo step (`EditHistory::beginGroup`
//   with `amends`), so one undo goes back to where the hand started;
// - Escape, a click elsewhere, a non-number or an unchanged Enter put the pre-reset value back and leave
//   NO step (`EditHistory::retractLast`) — a triple-click then Escape changes nothing at all.
//
// In mapping mode the double-click unroutes instead, and the triple types the ROUTE AMOUNT as a percent,
// the route list's own vocabulary (`device::parseAmountPercent`).
//
// **ONE FIELD PER WIDGET, MADE ON FIRST USE AND THEN ONLY EVER HIDDEN** (`text_field.h`'s rule: never
// destroyed from its own callbacks). Lazily, because the editor holds hundreds of knobs and most are never
// typed into; positioned the moment it exists, and re-seated unconditionally on every layout and scale
// change (CLAUDE.md's scale rule).

namespace feathers::ui {

class EditHistory;
class TextField;

/// The history a control records its steps into, or null — see `RecordsEdits`.
[[nodiscard]] EditHistory* historyOf(const device::Modulatable& control);

class TypeIn {
public:
	explicit TypeIn(visage::Frame& owner);
	~TypeIn();
	TypeIn(const TypeIn&) = delete;
	TypeIn& operator=(const TypeIn&) = delete;

	/// **THE DOUBLE-CLICK, BEFORE ITS RESET** (or, with `amount`, before its unroute): remember what the
	/// reset is about to replace, the text the field will open on, and where the undo stack stood.
	/// `typeable` false refuses whatever the control says — a widget showing the control through a reading
	/// of its own (a value box's formatter printing semitones while a note is tracked).
	void beginReset(device::Modulatable& control, bool amount, bool typeable = true);
	/// **AND AFTER IT** — whether the reset recorded a step, and which.
	void endReset();
	/// **WHAT THE OWNER READS A TYPED TEXT AS, before the control parses it** — a box whose face prints the control's own
	/// number under another unit name (`ValueBox::setReadoutFormatter`'s `sameNumber`) takes that name back off.
	/// Unset, the text goes to the control as typed.
	void setRewrite(std::function<std::string(const std::string&)> rewrite) { rewrite_ = std::move(rewrite); }
	/// Any other press on the owner: what a second click armed is no longer a triple's.
	void disarm() noexcept { armed_ = false; }

	/// **THE THIRD CLICK: OPEN OVER `bounds`** (the owner's coordinates). False, and nothing opens, when no
	/// reset was armed or the control refuses (`Modulatable::editText`).
	bool open(visage::Bounds bounds);
	/// Re-seat over `bounds`. Unconditional, so a scale change re-converts the native size.
	void place(visage::Bounds bounds);
	/// **A PRESS ON THE OWNER WHILE THE FIELD IS OPEN** — the house's click-elsewhere, a cancel. True when
	/// there was a field to close.
	bool closeIfOpen();

	[[nodiscard]] bool editing() const noexcept;
	/// The field, or null before the first triple-click.
	[[nodiscard]] TextField* field() const noexcept { return field_.get(); }
	/// Whether the control behind the last double-click refused to be typed into.
	[[nodiscard]] bool refused() const noexcept { return armed_ && !opening_; }

private:
	void commit(const std::string& text);
	void cancel();
	/// Write `after` (a value, or an amount) through the control's own gesture, folded into the reset's
	/// step when that step is still the tip — and say whether it was.
	bool write(double after);
	[[nodiscard]] double current() const;
	void forget() noexcept;

	visage::Frame& owner_;
	std::unique_ptr<TextField> field_;
	visage::Bounds bounds_;

	device::Modulatable* control_ = nullptr;
	EditHistory* history_ = nullptr;
	bool amount_ = false;
	bool armed_ = false;
	bool open_ = false;
	double before_ = 0.0;
	std::optional<std::string> opening_;
	/// See `setRewrite`.
	std::function<std::string(const std::string&)> rewrite_;
	uint64_t serialBefore_ = 0;
	/// The step the reset recorded, or 0 when it recorded none.
	uint64_t resetSerial_ = 0;
};

} // namespace feathers::ui
