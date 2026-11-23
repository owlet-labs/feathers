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

#include "plugin/ui/generic_card.h"

namespace feathers::ui {

/// PITCH'S CARD: the generic grid, with Shift and Fine's hover text following the Scale (`fx/pitch/pitch_tips.h`).
class PitchCard final : public GenericCard {
public:
	PitchCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr);
	PitchCard(backplane::DeviceHost&, size_t, Notifier* = nullptr) = delete;

protected:
	void refreshTips() override;

private:
	/// WHETHER THE TEXTS ON THE CONTROLS ARE SET FOR A SCALE: -1 until the first tick, 0 for Free, 1 for a Scale.
	int drawnScaled_ = -1;
};

} // namespace feathers::ui
