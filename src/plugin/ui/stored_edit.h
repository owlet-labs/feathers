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

#include "plugin/ui/edit_history.h"

#include <functional>
#include <memory>
#include <utility>

// ONE STORED VALUE THAT IS NOT A PARAMETER, CHANGED — for the editor's one history (backlog JB, 2026-10-11).
//
// The patch keeps state no host parameter carries: the drawer's switches and curve, a macro's name, polarity
// and type, an output's label. Each is written through one bridge or host verb that already refuses a write
// of the value it holds, so the record is that verb, the value before, and the value after.
//
// **THE WRITER CAPTURES THE BRIDGE OR HOST AND AN ADDRESS (a slot, a device id, an index) — NEVER A WIDGET.**
// edit_history.h's rule: an undo walked after the panel is gone must still land. The bridge and the
// Feathers host outlive the editor's history (`Gui::Impl` clears it in teardown).

namespace feathers::ui {

template <typename T>
class StoredEdit final : public EditRecord {
public:
	using Write = std::function<void(const T&)>;

	StoredEdit(Write write, T before, T after, const char* verb)
	    : write_(std::move(write)), before_(std::move(before)), after_(std::move(after)), verb_(verb) {}

	void undo() const override { write_(before_); }
	void redo() const override { write_(after_); }
	/// A string literal from the call site, as every record's verb is.
	const char* verb() const override { return verb_; }

private:
	Write write_;
	T before_;
	T after_;
	const char* verb_;
};

/// **WRITE `after` AS ONE UNDO STEP**: read what is there, write, read again, and record only if the stored
/// value moved — a write the verb refused, or of the value already held, costs no step. `history` may be null
/// (a surface built with none records nothing). True if it moved.
template <typename T, typename Read>
bool writeStoredAsEdit(EditHistory* history, Read read, typename StoredEdit<T>::Write write, T after,
                       const char* verb) {
	T before = read();
	write(after);
	T now = read();
	if (now == before) {
		return false;
	}
	if (history != nullptr) {
		history->record(std::make_unique<StoredEdit<T>>(std::move(write), std::move(before), std::move(now), verb));
	}
	return true;
}

} // namespace feathers::ui
