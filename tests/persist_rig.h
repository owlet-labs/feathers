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

// **THE PERSISTENCE RIG** (backlog NM): a rack and its bridge, run a block at a time the way a host runs them — drain,
// render, publish — so a case can capture, save to text, load into a FRESH rack, and compare the device's own state
// value by value. One rig for every device's case, so they cannot each test a different road.

#include "doctest.h"
#include "plugin/device_registry.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"

#include <array>
#include <memory>
#include <span>
#include <string>

namespace feathers::persist {

struct Rig {
	static constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4,
		                                     .outputStride = 8 };
	static constexpr double kRate = 48000.0;
	rack::RackEngine engine;
	std::unique_ptr<rack::RackBridge> bridge;
	DeviceInstanceId id = kNoInstance;
	std::array<float, 64> l {};
	std::array<float, 64> r {};

	/// `axes`: the rig's own by default (a source bound of 8); a case about a wider source passes the rack's.
	explicit Rig(device::Catalogue catalogue = hostCatalogue(), const backplane::Axes& axes = kAxes) : engine(axes, catalogue) {
		engine.prepare(kRate, 4096);
		bridge = std::make_unique<rack::RackBridge>(engine);
	}
	/// One device, its controls from `row` (the device's own row encoding).
	void seat(const char* tag, std::span<const float> row = {}) {
		id = bridge->addDevice(tag);
		REQUIRE(id != kNoInstance);
		for (size_t i = 0; i < row.size(); ++i) {
			bridge->setField(id, i, row[i]);
		}
	}
	void block(int times = 1) {
		for (int i = 0; i < times; ++i) {
			bridge->drain();
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			bridge->publish(engine);
		}
	}
	/// THE RUNNING INSTANCE'S STORAGE — the device's own object, for a case to read its state from.
	[[nodiscard]] const void* storage() const {
		const size_t lane = engine.slotOf(id);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		return engine.slotAtLane(lane).storage.get();
	}
	template <class T> [[nodiscard]] const T& as() const { return *static_cast<const T*>(storage()); }
	[[nodiscard]] std::string save() const {
		std::string text;
		REQUIRE(rack::rackToText(bridge->patch(), &text));
		return text;
	}
	/// THE PATCH INTO THIS RIG, and the drain that adopts what it built.
	void load(const std::string& text) {
		rack::RackPatch patch;
		REQUIRE(rack::parseRackText(text, &patch));
		(void)bridge->loadPatch(patch);
		bridge->drain();
		id = bridge->deviceAtPosition(0);
		REQUIRE(id != kNoInstance);
	}
};

} // namespace feathers::persist
