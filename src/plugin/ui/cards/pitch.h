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

#include "fx/pitch/pitch_types.h"
#include "plugin/ui/card_factory.h"
#include "plugin/ui/pitch_card.h"

// This device's card, registered from its own file (backlog NY.1), named by the device's device.cmake
// (CARD_INCLUDE / CARD_REG) so the generated table picks it up. Pitch's card is the generic grid with the
// Scale's hover text on Shift and Fine (TV audit, 2026-11-22).

namespace feathers::ui::cards {

inline constexpr CardReg kPitchCard {
	fx::pitch::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry&, DeviceInstanceId s, Notifier* n, ModState*,
	     EditHistory*) -> std::unique_ptr<FxCard> {
		  return std::make_unique<PitchCard>(h, s, n);
	  },
	nullptr };

} // namespace feathers::ui::cards
