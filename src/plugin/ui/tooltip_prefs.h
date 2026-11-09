/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/editor_prefs.h"
#include "plugin/ui/tooltip.h"

#include <memory>
#include <string>

// THE TOOLTIP DETAIL, KEPT IN AN EDITOR'S PREFS — the preset root's two halves (`preset_browser.h`,
// `restorePresetRoot` / `keepPresetRootIn`), for the one setting `tooltip.h` declares. Apart from that
// file because the tooltip layer is compiled into the device-UI probe, which may not reach the plugin's
// prefs: a card asks `tooltipDetail()` and never learns where it is kept.
//
// **A SETTINGS PAGE BINDS `ui::setTooltipDetail`** (a setting meant for a settings page later) —
// a Brief | Full choice showing both on its face. That call changes the detail for
// the process and every editor's watch writes it to its prefs; nothing here needs to change.

namespace feathers::ui {

/// The key the editors keep the detail under: `brief` or `full`. Anything else — including no line at
/// all — reads as FULL, the default (`tooltipDetail`).
inline constexpr const char* kTooltipDetailPrefKey = "tooltips.detail";

[[nodiscard]] inline TooltipDetail tooltipDetailIn(const EditorPrefs& prefs) {
	return prefs.text(kTooltipDetailPrefKey) == "brief" ? TooltipDetail::Brief : TooltipDetail::Full;
}

/// At an editor's startup: the stored detail becomes the process's.
inline void restoreTooltipDetail(const EditorPrefs& prefs) { (void)setTooltipDetail(tooltipDetailIn(prefs)); }

/// For an editor's lifetime: every `setTooltipDetail` in the process is written to `prefs`.
[[nodiscard]] inline std::unique_ptr<TooltipDetailWatch> keepTooltipDetailIn(EditorPrefs& prefs) {
	return std::make_unique<TooltipDetailWatch>([&prefs](TooltipDetail detail) {
		prefs.setText(kTooltipDetailPrefKey, detail == TooltipDetail::Full ? "full" : "brief");
	});
}

} // namespace feathers::ui
