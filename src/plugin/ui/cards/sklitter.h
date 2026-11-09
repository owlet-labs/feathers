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
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/sklitter_card.h"

// This device's card, registered from its own file (backlog NY.1): the entry that used to stand in
// card_factory.cpp's hand-written kCards, moved here verbatim. The device's device.cmake names it
// (CARD_INCLUDE / CARD_REG) and the generated table picks it up.

namespace feathers::ui::cards {

// **SKLITTER — the band above the list** (a flagship effect, so it gets the extra effort to make it
// beautiful). It derives from `GenericCard` rather than replacing it:
// the list below the band is the same one-column, scrolling, mode-aware list, and a hand-written
// copy of that machinery would be a second implementation whose first divergence nobody notices.
inline constexpr CardReg kSklitterCard {
	fx::sklitter::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry&, DeviceInstanceId s, Notifier* n, ModState*,
	     EditHistory*) -> std::unique_ptr<FxCard> {
		  return std::make_unique<SklitterCard>(h, s, fx::sklitter::kDescriptor, n);
	  },
	nullptr };

} // namespace feathers::ui::cards
