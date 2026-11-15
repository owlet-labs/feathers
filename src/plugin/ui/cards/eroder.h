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

#include "plugin/ui/card_factory.h"
#include "fx/eroder/eroder_types.h"
#include "plugin/ui/eroder_card.h"

// This device's card, registered from its own file: the device's device.cmake names it (CARD_INCLUDE / CARD_REG)
// and the generated table picks it up.

namespace feathers::ui::cards {

// ERODER: the spectrum and the band as the centrepiece, or the Comb's eleven bars (`eroder_card.h`).
inline constexpr CardReg kEroderCard {
	fx::eroder::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry& r, DeviceInstanceId s, Notifier* n, ModState*,
	     EditHistory*) -> std::unique_ptr<FxCard> {
		  return std::make_unique<EroderCard>(h, r, s, n);
	  },
	nullptr };

} // namespace feathers::ui::cards
