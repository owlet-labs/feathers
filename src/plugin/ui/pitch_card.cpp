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

#include "plugin/ui/pitch_card.h"

#include "fx/pitch/pitch_tips.h"
#include "fx/pitch/pitch_types.h"

#include <cmath>

namespace feathers::ui {

namespace pt = fx::pitch;

PitchCard::PitchCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : GenericCard(host, id, pt::kDescriptor, notifier) {}

void PitchCard::refreshTips() {
	// EVERY ROAD THAT MOVES THE SCALE is a tick that finds it moved: a click, automation, a preset or undo. The texts move
	// only when the scale crosses Free.
	const auto scale = static_cast<long>(std::lround(ownHost().fieldAt(deviceId(), static_cast<size_t>(pt::Param::Scale))));
	const int scaled = scale != 0 ? 1 : 0;
	if (scaled == drawnScaled_) {
		return;
	}
	drawnScaled_ = scaled;
	const pt::ScaleTips& tips = pt::tipsFor(scaled != 0);
	control(static_cast<size_t>(pt::Param::Shift)).setTooltipOverride(tips.shift);
	control(static_cast<size_t>(pt::Param::Fine)).setTooltipOverride(tips.fine);
}

} // namespace feathers::ui
