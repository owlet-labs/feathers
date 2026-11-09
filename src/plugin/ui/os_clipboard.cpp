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

#include "plugin/ui/os_clipboard.h"

#include "rack/fragment.h"

#include <visage/app.h>

namespace feathers::ui {

namespace {
int bound = 0;
}

void bindOsClipboard() {
	if (bound++ == 0) {
		rack::setOsClipboard({ [] { return visage::readClipboardText(); },
		                       [](const std::string& text) { visage::setClipboardText(text); } });
	}
}

void unbindOsClipboard() {
	if (bound > 0 && --bound == 0) {
		rack::clearOsClipboard();
	}
}

} // namespace feathers::ui
