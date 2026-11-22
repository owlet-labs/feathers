/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "backplane/inner_routes.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"

#include <string>
#include <vector>

namespace feathers::ui {

// **THE ROUTES LOCAL TO ONE RACK, AS A HOST FOR THE ROUTE LIST.** A rack's macro n reaches devices inside its chains through the chains' own
// route tables (each chain's own macro n, whose value the rack drives). `RouteList` is one widget over `DeviceHost`; this adapter gives it the
// rack's view of that: no routes of its own, and for the armed macro n every chain's routes from its macro n, edited through that chain's own
// bridge and gate. Everything else (names of ends, the sample rate, the fields) is the enclosing host's, forwarded.
//
// A row's `chain` is the RACK's device id and its destination is in the chains' shared id space, which is how the chain is found again
// (`Rack::holding`). Message thread.
class RackRouteHost final : public backplane::DeviceHost {
public:
	RackRouteHost(backplane::DeviceHost& host, DeviceInstanceId rack) : host_(host), rack_(rack) {}

	using backplane::DeviceHost::fieldAt;
	using backplane::DeviceHost::setField;
	using backplane::DeviceHost::fieldsPresentAt;
	using backplane::DeviceHost::readingCountAt;
	using backplane::DeviceHost::readingAt;
	using backplane::DeviceHost::descriptorOf;

	const backplane::Axes& axes() const noexcept override { return host_.axes(); }
	backplane::DeviceView devices() const noexcept override { return host_.devices(); }
	float fieldAt(DeviceInstanceId device, size_t control) const override { return host_.fieldAt(device, control); }
	void setField(DeviceInstanceId device, size_t control, float value) override { host_.setField(device, control, value); }
	size_t fieldsPresentAt(DeviceInstanceId device) const override { return host_.fieldsPresentAt(device); }
	double sampleRate() const noexcept override { return host_.sampleRate(); }
	size_t readingCountAt(DeviceInstanceId device) const noexcept override { return host_.readingCountAt(device); }
	float readingAt(DeviceInstanceId device, size_t index) const noexcept override { return host_.readingAt(device, index); }

	// THE HOST'S OWN ROUTES ARE NOT THIS LIST'S: none from any source.
	float routeAmount(backplane::Source, backplane::Destination) const override { return 0.0f; }
	void setRouteAmount(backplane::Source, backplane::Destination, float) override {}
	bool routedElsewhere(backplane::Destination, backplane::Source) const override { return false; }
	void beginRouteGesture() override { host_.beginRouteGesture(); }
	void endRouteGesture() override { host_.endRouteGesture(); }
	void forEachRouteFrom(backplane::Source, void*, void (*)(void*, backplane::Destination)) const override {}
	std::string destinationName(backplane::Destination destination) const override { return host_.destinationName(destination); }
	bool supportsRouteTransfers() const override { return true; }
	bool sourceIsBipolarNow(backplane::Source source) const override { return host_.sourceIsBipolarNow(source); }

	/// EVERY CHAIN'S ROUTES FROM ITS OWN MACRO `source` (a macro is `Source::core(n)`; anything else has none here).
	std::vector<InnerRoute> innerRoutesFrom(backplane::Source source) const override {
		std::vector<InnerRoute> out;
		const rack::parallel::Rack* rack = rackNow();
		if (rack == nullptr || !isMacro(source)) {
			return out;
		}
		for (size_t i = 0; i < rack->chainCount(); ++i) {
			const rack::RackBridge* bridge = rack->chainAt(i) != nullptr ? rack->chainAt(i)->bridge() : nullptr;
			if (bridge == nullptr) {
				continue;
			}
			bridge->forEachRoute([&](const backplane::Route& r) {
				if (r.source == source) {
					out.push_back(InnerRoute { rack_, r.destination, r.amount });
				}
			});
		}
		return out;
	}
	float innerRouteAmount(backplane::Source source, DeviceInstanceId, backplane::Destination inner) const override {
		const rack::RackBridge* bridge = bridgeHolding(inner);
		return bridge != nullptr ? bridge->routeAmount(source, inner) : 0.0f;
	}
	bool setInnerRouteAmount(backplane::Source source, DeviceInstanceId, backplane::Destination inner, float amount) override {
		rack::RackBridge* bridge = bridgeHolding(inner);
		if (bridge == nullptr || (amount != 0.0f && !bridge->fragmentRecord().mayRoute(source, inner))) {
			return false;
		}
		bridge->setRouteAmount(source, inner, amount);
		return true;
	}
	backplane::Transfer innerRouteTransfer(backplane::Source source, DeviceInstanceId, backplane::Destination inner) const override {
		const rack::RackBridge* bridge = bridgeHolding(inner);
		return bridge != nullptr ? bridge->routeTransfer(source, inner) : backplane::Transfer::Lin;
	}
	bool setInnerRouteTransfer(backplane::Source source, DeviceInstanceId, backplane::Destination inner, backplane::Transfer t) override {
		rack::RackBridge* bridge = bridgeHolding(inner);
		return bridge != nullptr && bridge->setRouteTransfer(source, inner, t);
	}
	/// "Kick · Filter · Cutoff": the chain's label first, as the rack names a route into it.
	std::string innerDestinationName(DeviceInstanceId, backplane::Destination inner) const override {
		const rack::parallel::Rack* rack = rackNow();
		const device::Descriptor* desc = host_.descriptorOf(rack_);
		if (rack == nullptr || desc == nullptr || desc->container == nullptr || desc->container->nameDestination == nullptr) {
			return "Chain";
		}
		char buffer[128] = {};
		return desc->container->nameDestination(rack, inner, buffer, sizeof(buffer)) > 0 ? std::string(buffer) : std::string("Chain");
	}

private:
	[[nodiscard]] const rack::parallel::Rack* rackNow() const { return rack::parallel::rackOf(host_.containerSurfaceOf(rack_)); }
	[[nodiscard]] static bool isMacro(backplane::Source source) { return !source.isDevice() && !source.isOuter(); }
	[[nodiscard]] rack::RackBridge* bridgeHolding(backplane::Destination inner) const {
		const rack::parallel::Rack* rack = rackNow();
		rack::InnerChain* chain = rack != nullptr ? rack->holding(inner.device) : nullptr;
		return chain != nullptr ? chain->bridge() : nullptr;
	}

	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
};

} // namespace feathers::ui
