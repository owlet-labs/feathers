/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>

namespace feathers::mod {

/// **TABS: ONE DEVICE HOLDING SEVERAL WHOLE COPIES OF ITSELF** (the LFO's four, the Sample & Hold's four). The device's row is
/// its tabs end to end — tab 1 first, and tab 1 is the device as it was before it had tabs, so a patch from then is tab 1 and
/// nothing else. A tab's control `c` sits at `tab * perTab + c`; tabs 2 and up are APPENDED fields with prefixed keys and
/// start switched off. Each tab publishes its own output (`SourceDesc::enableControl`/`amountControl` name its own switch
/// and amount), and a tab that is off is not run at all.
inline constexpr size_t kTabs = 4;

/// THE ROW INDEX of control `control` on tab `tab` (0-based), for a device whose tab is `perTab` controls.
[[nodiscard]] constexpr size_t tabField(size_t perTab, size_t tab, size_t control) noexcept {
	return tab * perTab + control;
}

/// WHICH TAB, AND WHICH CONTROL OF IT, a row index is — the inverse of `tabField`.
struct TabControl {
	size_t tab;
	size_t control;
};
[[nodiscard]] constexpr TabControl tabControlOf(size_t perTab, size_t field) noexcept {
	return { field / perTab, field % perTab };
}

/// **WHETHER A TAB RUNS**: the first always does (it keeps ticking while off, as the device always did); a later tab only
/// while switched on. One rule, so the LFO and the Sample & Hold cannot answer it two ways.
[[nodiscard]] constexpr bool tabRuns(size_t tab, bool enabled) noexcept {
	return tab == 0 || enabled;
}

} // namespace feathers::mod
