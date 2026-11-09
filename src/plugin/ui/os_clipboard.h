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

namespace feathers::ui {

/// **THE EDITOR HANDS THE PASTEBOARD THE OPERATING SYSTEM'S CLIPBOARD** (backlog OA): while at least one editor is open,
/// copy and cut also put the clip on the system clipboard as text, and paste reads a clip of ours back from it
/// (`rack::setOsClipboard`). Counted, so two editors in one process (two plugin instances) bind and unbind in any order;
/// the last to close takes the road away. Message thread. Through visage's `readClipboardText`/`setClipboardText`,
/// which serve the X11 selection requests, so another application (or another plugin instance's process) can read it.
void bindOsClipboard();
void unbindOsClipboard();

} // namespace feathers::ui
