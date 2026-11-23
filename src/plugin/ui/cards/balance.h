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

#include "plugin/ui/card_factory.h"
#include "fx/balance/balance_types.h"
#include "plugin/ui/balance_card.h"

// This device's card, registered from its own file (backlog NY.1); its device.cmake names it (CARD_INCLUDE / CARD_REG).

namespace feathers::ui::cards {

inline constexpr CardReg kBalanceCard {
	fx::balance::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry& r, DeviceInstanceId s, Notifier* n, ModState*,
	     EditHistory*) -> std::unique_ptr<FxCard> {
		  return std::make_unique<BalanceCard>(h, r, s, n);
	  },
	nullptr };

} // namespace feathers::ui::cards
