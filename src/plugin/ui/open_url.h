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

// OPEN THE ABOUT PART'S ONE LINK IN THE DEFAULT BROWSER.
//
// Only the fixed `kPlugUrl` is ever accepted, so no user input, patch text or host string
// can reach the OS opener. Linux `xdg-open` and macOS `open` are posix_spawn'd with a fixed argv (no
// shell, no command string); Windows calls ShellExecuteW. The child is not waited on, so the main thread
// never blocks; a failure to launch is silent (there is nothing to do about a desk with no browser).

namespace feathers::ui {

inline constexpr const char* kPlugUrl = "https://soundcloud.com/greatowlmusic";

/// Opens `url` in the default browser. Refuses (returns false) anything but an https://soundcloud.com/greatowlmusic
/// link, so no caller can turn it into a general launcher. The screen passes `kPlugUrl` and nothing else.
bool openInBrowser(const char* url);

} // namespace feathers::ui
