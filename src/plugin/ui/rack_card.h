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

// `RackCard` IS GONE — THERE IS ONE CARD BASE AND IT IS `ui::FxCard`.
//
// **IT WAS A SECOND BASE CARRYING A CONTRACT THAT SAID, IN ITS OWN COMMENT, THAT IT WAS THE SAME
// CONTRACT.** `RackCard::update()` documented itself as *"the same contract `FxCard::update()`
// carries"*, and `bodyHeight()` was the only thing it added. Two bases, one job.
//
// **AND THE COST WAS NOT DUPLICATION, IT WAS INCOMPATIBILITY.** `EnvCard` and `MsegCard` were
// `RackCard`s; the rack's lane holds `FxCard`s. So a modulator could not appear in the rack whatever
// else was ported to the `DeviceHost` seam — the modulator effects did not render. A
// parallel contract is not merely untidy: it is a wall between two hosts that were meant to share
// their cards.
//
// `bodyHeight()` moved to `FxCard` as a default of `laneHeight()` and has since been DELETED: the
// head division it named stopped existing when every card began framing itself, and a second
// width-blind way to ask a card its height is a second way to be told about a card nothing has placed
// yet. `FxCard::laneExtent(cross)` is the one question. This header remains only so that an include of
// it keeps compiling; it declares nothing.

#pragma once

#include "plugin/ui/fx_card.h"
