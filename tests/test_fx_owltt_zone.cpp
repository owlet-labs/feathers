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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/zone_param.h"

#include <cmath>

// THE ZONE MODEL MOVED TO `src/device/` so a shared widget can reach it without including a device
// — `feathers::device` cannot depend on `feathers::fx::owltt`. The namespace moved with it, and this
// line is what the move forgot: the header changed its home and the header's USERS did not.
using namespace feathers;

TEST_CASE("zone computation maps 0..1 to the correct zone and position") {
	constexpr int32_t kNumZones = 8;

	SUBCASE("zone boundaries land where expected") {
		// At exactly 0, we're at the start of zone 0.
		ZoneInfo z0 = computeZone(0.0f, kNumZones);
		CHECK(z0.index == 0);
		CHECK(z0.position == doctest::Approx(0.0f).epsilon(0.001f));

		// At 0.125 (1/8), we're at the start of zone 1.
		ZoneInfo z1 = computeZone(0.125f, kNumZones);
		CHECK(z1.index == 1);
		CHECK(z1.position == doctest::Approx(0.0f).epsilon(0.001f));

		// At 1.0, we're at the end of the last zone.
		ZoneInfo z8 = computeZone(1.0f, kNumZones);
		CHECK(z8.index == 7);
	}

	SUBCASE("mid-zone values report the correct position") {
		// Halfway through zone 0: value = 0.0625, position = 0.5.
		ZoneInfo z = computeZone(0.0625f, kNumZones);
		CHECK(z.index == 0);
		CHECK(z.position == doctest::Approx(0.5f).epsilon(0.001f));

		// Halfway through zone 3: value = 0.4375, position = 0.5.
		ZoneInfo z3 = computeZone(0.4375f, kNumZones);
		CHECK(z3.index == 3);
		CHECK(z3.position == doctest::Approx(0.5f).epsilon(0.001f));
	}

	SUBCASE("out-of-range values are clamped") {
		ZoneInfo neg = computeZone(-0.5f, kNumZones);
		CHECK(neg.index == 0);

		ZoneInfo over = computeZone(1.5f, kNumZones);
		CHECK(over.index == 7);
	}
}

TEST_CASE("ZoneBasedParam provides zone semantics for a stored value") {
	constexpr int32_t kNumZones = 8;

	ZoneBasedParam<kNumZones> param;
	param.value = 0.5f;

	SUBCASE("reports correct zone index and position") {
		CHECK(param.getZoneIndex() == 4);
		CHECK(param.getPosInZone() == doctest::Approx(0.0f).epsilon(0.001f));
	}

	SUBCASE("isInZoneOrLater works correctly") {
		CHECK(param.isInZoneOrLater(0));
		CHECK(param.isInZoneOrLater(4));
		CHECK_FALSE(param.isInZoneOrLater(5));
	}
}
