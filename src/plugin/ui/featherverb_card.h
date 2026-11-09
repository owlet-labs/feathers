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

#include "plugin/ui/featherverb_meter.h"
#include "plugin/ui/generic_card.h"

// FEATHERVERB'S CARD — the generic grid, and the meter beside Input (backlog LA). The face's last row is Input
// alone (`kFaceOrder`), and the meter (`featherverb_meter.h`) takes the rest of that row: the gain and what it
// does, side by side, in room the grid already left. Nothing else about the grid changes, so this derives from
// `GenericCard` and uses only its `drawBodyExtras` hook.

namespace feathers::ui {

class FeatherverbCard final : public GenericCard {
public:
	FeatherverbCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr);

	bool updateContent() override;

	/// TEST SEAMS — the meter as drawn: its fractions, off this card's own readings, and where it sits.
	[[nodiscard]] featherverb_meter::Fractions meterForTest() const {
		return featherverb_meter::read(ownHost(), deviceId());
	}
	[[nodiscard]] visage::Bounds meterBoundsForTest() const { return meterBounds(); }

protected:
	void drawBodyExtras(visage::Canvas& canvas) override;

private:
	/// THE CELLS RIGHT OF INPUT'S, in its row; empty while Input is not laid out (a folded card).
	[[nodiscard]] visage::Bounds meterBounds() const;

	featherverb_meter::Fractions drawn_ {};
};

} // namespace feathers::ui
