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

#pragma once

#include "backplane/device_host.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/text_field.h"

#include <memory>
#include <string>
#include <visage/ui.h>

namespace feathers::ui {

/// **RENAMING A DEVICE INSTANCE, FROM ITS HEADER** — one helper every card host holds (a chain column, the rack editor, the fx lane): the
/// "Rename…" row of the slot menu, and the inline field over the header's title. Enter commits (an empty text clears the name), Esc
/// cancels; the field is the surface's child, hidden between uses, above every sibling (`setOnTop`) and is the same `TextField` every
/// rename in the editor uses, so the key routing guards the others rely on (a focused field takes its keys first) hold here too.
/// The name goes to the host (`DeviceHost::setUserName`) as ONE undo step.
class DeviceRename {
public:
	/// The row ids this helper adds to a slot menu, and answers in `pick`.
	static constexpr int kRenameRow = 7001;
	static constexpr int kClearRow = 7002;

	DeviceRename(visage::Frame& surface, Notifier* notifier);
	~DeviceRename();
	DeviceRename(const DeviceRename&) = delete;
	DeviceRename& operator=(const DeviceRename&) = delete;

	void setHistory(EditHistory* history) noexcept { history_ = history; }
	/// "Rename…" and, while it has a name, "Clear name": appended to a slot menu.
	void addRows(visage::PopupMenu& menu, const backplane::DeviceHost& host, DeviceInstanceId device) const;
	/// A menu pick: true when it was one of this helper's rows (and was acted on). `source`/`anchor` place the field over the header.
	bool pick(int row, backplane::DeviceHost& host, DeviceInstanceId device, visage::Frame& source, const visage::Bounds& anchor);
	/// The field over `anchor` (in `source`'s coordinates), holding the current name (else the declared one).
	void begin(backplane::DeviceHost& host, DeviceInstanceId device, visage::Frame& source, const visage::Bounds& anchor);
	[[nodiscard]] bool renaming() const { return field_->editing(); }
	[[nodiscard]] TextField& field() const noexcept { return *field_; }
	/// A commit as Enter makes it, for the device the field was opened on; false when it changed nothing or the device is gone.
	bool commitForTest(const std::string& text) { return commit(text); }
	[[nodiscard]] DeviceInstanceId deviceForTest() const noexcept { return device_; }

private:
	bool commit(const std::string& text);

	visage::Frame& surface_;
	Notifier* notifier_ = nullptr;
	EditHistory* history_ = nullptr;
	std::unique_ptr<TextField> field_;
	backplane::DeviceHost* host_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;
};

} // namespace feathers::ui
