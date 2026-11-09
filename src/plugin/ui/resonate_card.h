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
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/resonate_face.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// RESONATE'S CARD — the string at its heart, the five pictograms over it, six knobs under it, and nothing
// else (the card was far too big, so it is greatly simplified); everything else is on the
// panel. The widgets are `ResonateFace`'s, built for the card surface; this is the FxCard around them: the
// head, the fold and the lane height. `ClipCard` is the template it follows.

namespace feathers::ui {

class ResonateCard final : public FxCard {
public:
	ResonateCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	             Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — see `GenericCard`.
	ResonateCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::resonate::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	/// THE OPEN HEIGHT FOR A CARD `cardWidth` WIDE — the face's own column arithmetic.
	[[nodiscard]] float openHeightAt(float cardWidth) const;

	[[nodiscard]] const ResonateFace& face() const { return *face_; }
	[[nodiscard]] std::vector<fx::resonate::Param> addressedParamsForTest() const;
	[[nodiscard]] visage::Frame* widgetForTest(fx::resonate::Param p) const { return face_->widgetFor(p); }

private:
	device::Modulatable* enableModulatable() override;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	std::unique_ptr<ResonateFace> face_;
};

} // namespace feathers::ui
