/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/card_factory.h"
#include "plugin/ui/twinfield_card.h"

namespace feathers::ui::cards {

inline constexpr CardReg kTwinFieldCard {
	mod::twinfield::kTraits.tag,
	[](backplane::DeviceHost& h, WidgetRegistry& r, DeviceInstanceId s, Notifier* n, ModState* m,
	   EditHistory* hist) -> std::unique_ptr<FxCard> { return std::make_unique<TwinFieldCard>(h, r, s, n, m, hist); },
	[](size_t outputRows) { return TwinFieldCard::openHeight(outputRows); } };

} // namespace feathers::ui::cards
