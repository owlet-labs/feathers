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
#include "mod/dahdsr/dahdsr_types.h"
#include "plugin/ui/env_card.h"

// This device's card, registered from its own file (backlog NY.1): the entry that used to stand in
// card_factory.cpp's hand-written kCards, moved here verbatim. The device's device.cmake names it
// (CARD_INCLUDE / CARD_REG) and the generated table picks it up.

namespace feathers::ui::cards {

// THE MODULATOR TIER, AND THE REASON IT WAS MISSING FOR SO LONG. `EnvCard` and `MsegCard` took a
// `ParamBridge&` — Feathers' own road — so they could not be built by the rack whatever this table
// said, and the modulator devices did not render. Both now take `backplane::DeviceHost`,
// which is what lets one selection serve both hosts.
//
// **THEY DECLINE WITHOUT MODULATION STATE RATHER THAN HOLDING A DANGLING REFERENCE.** A null there
// means "this host has nothing to arm with", and the caller gets the generic grid — the same shape
// of honest refusal `makeExpandedView` makes when it is handed no parameter road.
inline constexpr CardReg kDahdsrCard {
	mod::dahdsr::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry&, DeviceInstanceId s, Notifier*, ModState* m,
	     EditHistory* hist) -> std::unique_ptr<FxCard> {
		  return (m != nullptr && hist != nullptr) ? std::make_unique<EnvCard>(h, s, *m, *hist)
		                                           : nullptr;
	  },
	nullptr };

} // namespace feathers::ui::cards
