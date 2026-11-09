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

#include "mod/arp/arp_types.h"
#include "plugin/ui/arp_card.h"
#include "plugin/ui/generic_expanded.h"
#include "plugin/ui/source_arm.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE ARP'S WINDOW — `GenericExpanded`'s grid of every control, under a band holding
// what the card shows beside its controls: the STEP LANE, larger, and the three SOURCE HANDLES (→ Step, → Pitch,
// → Velocity). Window ⊇ card is about everything the card offers, so a window without the picture would be a downgrade.
//
//     ┌──────────────────────────────────────────────────┐
//     │  ▮   ▮▮  ▮   ▮▮  ▮   ▮▮  ▮   ▮▮  ▮   ▮▮  ▮▌       │   the lane
//     └──────────────────────────────────────────────────┘
//     → Step   → Pitch   → Velocity                          the handles, the card's own walk (`outputChipBoundsFor`)
//     Time Base  Rate  Order  Octaves  …                      every control

namespace feathers::ui {

class ArpExpanded final : public GenericExpanded {
public:
	ArpExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	~ArpExpanded() override;

	void resized() override;
	[[nodiscard]] bool updateContent() override;

	static constexpr float kLaneHeight = 112.0f;
	static constexpr float kBandGap = 6.0f;

	[[nodiscard]] ArpLane& lane() const { return *lane_; }
	/// THE SOURCE HANDLES THIS WINDOW CARRIES, by output index — null until a modulation state is bound (a handle with
	/// nothing to arm against is not drawn, the strip's rule).
	[[nodiscard]] const SourceArm* armForTest(size_t output) const {
		return output < arms_.size() ? arms_[output].get() : nullptr;
	}

protected:
	[[nodiscard]] float bodyTopInset() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;

private:
	void layOutBand();

	std::unique_ptr<ArpLane> lane_;
	std::array<std::unique_ptr<SourceArm>, std::size(mod::arp::kOutputs)> arms_ {};
};

} // namespace feathers::ui
