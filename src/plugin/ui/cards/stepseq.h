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
#include "mod/stepseq/stepseq_types.h"
#include "plugin/ui/stepseq_card.h"

// This device's card, registered from its own file (backlog NY.1): the entry that used to stand in
// card_factory.cpp's hand-written kCards, moved here verbatim. The device's device.cmake names it
// (CARD_INCLUDE / CARD_REG) and the generated table picks it up.

namespace feathers::ui::cards {

// **THE STEP SEQUENCER (backlog MX) — the card is the step grid** (`stepseq_card.h`). It never declines, for
// MATH's reason; without a history its grid edits record nothing, and `setModulation` hands one over later.
inline constexpr CardReg kStepseqCard {
	mod::stepseq::kTraits.tag,
	  [](backplane::DeviceHost& h, WidgetRegistry&, DeviceInstanceId s, Notifier* n, ModState* m,
	     EditHistory* hist) -> std::unique_ptr<FxCard> {
		  return std::make_unique<StepSeqCard>(h, s, n, m, hist);
	  },
	[](size_t outputRows) { return StepSeqCard::openHeight(outputRows); } };

} // namespace feathers::ui::cards
