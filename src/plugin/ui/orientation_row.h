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

#include "plugin/gui_bridge.h"

#include <visage/ui.h>

#include <functional>

// The global rotate pin as a bare slider, for the debug panel only.
//
// ONE SLIDER NOW, because the orientation is one number. M(rotate, home) reads a single turn; the
// aperture and the other five angles the old decode carried are unread, and the rows that showed
// them showed authority they did not have. What is left is the one handle the audition question
// still needs: pin every sound to a deliberate turn, sweep it live, compare.
//
// A sound's rotate is normally its own — the Explore corner turns it per sound — so this is an
// override rather than the source: touching it pins a turn for every sound, and right-clicking
// releases it back to each sound's own state. Unpinned reads 0, and that is not a stale default:
// an unturned sound genuinely plays at rotate 0, the open plane its home gives it, so pinning
// from rest starts exactly where every unturned sound already is.
//
// NEVER A CLAP PARAMETER — only UI controls, kept for pinning values experimentally — which also
// keeps rotate non-automatable, as the design always had
// it. Added only under FEATHERS_DEBUG_PANEL, spending no id and touching nothing the patch saves
// beyond the pin itself.

namespace feathers::ui {

class OrientationRow : public visage::Frame {
public:
	OrientationRow(ParamBridge& bridge, std::function<void()> onChanged);

	void draw(visage::Canvas& canvas) override;

	/// What this row is showing: the pinned turn, or 0 unpinned — which is the turn every unturned
	/// sound is genuinely playing at, not a placeholder. Readable so a case can ask what the handle
	/// SAYS rather than read pixels.
	float value() const;

private:
	void applyFrom(const visage::MouseEvent& e);

	ParamBridge& bridge_;
	std::function<void()> onChanged_;
};

} // namespace feathers::ui
