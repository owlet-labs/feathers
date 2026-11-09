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
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#pragma once

#include "codec.h"
#include "mod/follower/follower_types.h"
#include "mod/follower/follower.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {

/// THE FOLLOWER, THROUGH ITS OWN DECODE (`settingsOf`, which the device's `setFields` runs): every control comes
/// back in its stored units, the enable included, so none needs an exemption.
inline RowVec tripFollower(const RowVec& in) {
	mod::follower::Follower f;
	f.prepare(48000.0);
	f.setFields(in.data(), in.size());
	const mod::follower::Settings& s = f.settings();
	RowVec out(static_cast<size_t>(mod::follower::Param::kCount), 0.0f);
	const auto at = [&](mod::follower::Param p) -> float& { return out[static_cast<size_t>(p)]; };

	at(mod::follower::Param::Mode) = static_cast<float>(s.mode);
	at(mod::follower::Param::Input) = s.input;
	at(mod::follower::Param::Detector) = static_cast<float>(s.detector);
	at(mod::follower::Param::Attack) = s.attackLog2;
	at(mod::follower::Param::Release) = s.releaseLog2;
	at(mod::follower::Param::Gain) = s.gainDb;
	at(mod::follower::Param::Floor) = s.floorDb;
	at(mod::follower::Param::Threshold) = s.threshold;
	at(mod::follower::Param::Poly) = s.poly ? 1.0f : 0.0f;
	at(mod::follower::Param::Enabled) = s.enabled ? 1.0f : 0.0f;
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kFollowerCodec { mod::follower::kTraits.tag, &tripFollower };

} // namespace feathers::codecs
