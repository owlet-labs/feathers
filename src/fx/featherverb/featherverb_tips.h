/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/featherverb/featherverb_types.h"

namespace feathers::fx::featherverb {

/// **WHAT A FEATHERVERB CONTROL DOES IN THE SIZE MODE AND CONV MODE IT IS ON** (backlog TV, the audit's Featherverb
/// sites). The declared tooltips stay the general text; the card and the window put this over them through
/// `DeviceControl::setTooltipOverride`, refreshed every tick from the two Choices, so every road that moves a mode
/// (a click, automation, a preset, undo) shows the right text. `nullptr` keeps the declared text.
///
/// Each sentence is read from the engine: Character's four groups from `Core::refreshCharacter` and Aurora's
/// `Aurora::set` (the shift's swing and rate, the diffusion and damping offsets); Predelay's Owl clause from the
/// servo's attack; Matrix's Aurora rate from the Size text; Conv Blend's three gates from `convBlend`.
inline const char* tipFor(Param p, int sizeMode, int convMode) {
	switch (p) {
	case Param::Character:
		switch (sizeMode) {
		case 0: // Mouse
		case 1: // Rabbit
		case 2: // Lake
		case 3: // Trees
			return "In the rooms, the diffusion and the density. It keeps turning, and the mark beside it shows which lap";
		case 4: // Feather
			return "In Feather, its stereo field. It keeps turning, and the mark beside it shows which lap";
		case 5: // Sky
		case 7: // Vast
			return "In Sky and Vast, where the tail builds and how it moves. It keeps turning, and the mark beside it "
			       "shows which lap";
		case 6: // Aurora
			return "In Aurora, how far its shift swings and how fast it wobbles, with a little more diffusion and damping. "
			       "It keeps turning, and the mark beside it shows which lap";
		case kOwlMenu:
			return "In Owl, how far and how fast the tail blooms. It keeps turning, and the mark beside it shows which lap";
		default:
			return nullptr;
		}
	case Param::Predelay:
		return sizeMode == kOwlMenu
		           ? "Up to 50 ms before the reverb starts. In Owl it also sets how fast the tail swells"
		           : "Up to 50 ms before the reverb starts";
	case Param::Matrix:
		return sizeMode == kAuroraMenu ? "In Aurora, how fast the tail climbs as it rings" : nullptr;
	case Param::ConvBlend:
		switch (convMode) {
		case kConvParallel:
			return "How much of the wet is the convolution: the reverb against the convolved input";
		case kConvSeries:
			return "How much of the wet is the convolution: the reverb against the convolved reverb";
		case kConvExciter:
			return "How much of what drives the reverb is the room rather than the dry input";
		default:
			return nullptr;
		}
	default:
		return nullptr;
	}
}

} // namespace feathers::fx::featherverb
