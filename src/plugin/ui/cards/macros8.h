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
#include "mod/macros8/macros8_types.h"
#include "plugin/ui/macros_card.h"

// This device's card: the MACROS card (`macros_card.h`) over this device's descriptor and shape. It never
// declines, for the MACROS card's reason: it is the only reader of the `Type n` params.

namespace feathers::ui::cards {

inline constexpr CardReg kMacros8Card {
	mod::macros8::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry&, DeviceInstanceId s, Notifier* n, ModState* m,
	     EditHistory* hist) -> std::unique_ptr<FxCard> {
		  return std::make_unique<MacrosCard>(h, s, n, m, hist, kMacros8Shape);
	  },
	[](size_t outputRows) { return MacrosCard::openHeight(outputRows, kMacros8Shape); } };

} // namespace feathers::ui::cards
