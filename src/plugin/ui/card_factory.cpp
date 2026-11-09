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

#include "plugin/ui/card_factory.h"

#include "card_includes.inc"

namespace feathers::ui {

namespace {

// THE CONVERGED SET, AND IT IS DELIBERATELY A LIST RATHER THAN A FLAG ON THE DEVICE.
//
// A device does not know whether its CARD has been ported off the parameter bridge — that is a fact
// about this layer, not about the DSP, and putting it in `Traits` would have every device carrying a
// field about somebody else's refactor.
//
// EVERY DEVICE IN THE CATALOGUE IS HERE NOW. The list that used to stand below — owltt needing meters,
// waypoint needing a sample rate, automod held in another worktree — is empty, and `FxControl` should
// be leaving the editor tree with it.
//   waypoint  — draws a filter response and needs `ParamBridge::sampleRate()`.
// Both gaps are additions to a permanent shared API, which is a design conversation rather than a
// thing to invent here.
const CardReg kCards[] = {
#include "card_table.inc"
};

const CardReg* entryFor(const device::Descriptor& desc) noexcept {
	for (const CardReg& e : kCards) {
		if (device::sameTag(desc.traits.tag, e.tag)) {
			return &e;
		}
	}
	return nullptr;
}

} // namespace

bool hasCustomCard(const device::Descriptor& desc) noexcept {
	return entryFor(desc) != nullptr;
}

StripHeightFn stripHeightFor(const device::Descriptor& desc) noexcept {
	const CardReg* entry = entryFor(desc);
	return entry != nullptr ? entry->stripHeight : nullptr;
}

std::unique_ptr<FxCard> makeCustomCard(backplane::DeviceHost& host, WidgetRegistry& registry,
                                       DeviceInstanceId id, const device::Descriptor& desc,
                                       Notifier* notifier, ModState* mod, EditHistory* history) {
	const CardReg* entry = entryFor(desc);
	return entry != nullptr ? entry->make(host, registry, id, notifier, mod, history) : nullptr;
}

} // namespace feathers::ui
