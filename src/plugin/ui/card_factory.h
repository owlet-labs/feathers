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

#include "backplane/device_host.h"
#include "device/registry.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/widget_registry.h"

#include <cstddef>
#include <memory>

// WHICH CARD A DEVICE GETS — one answer, for every host.
//
// **THIS EXISTS BECAUSE THE RACK SHOWED A KNOB GRID FOR EVERY DEVICE** instead of the custom UIs
// the devices offer. Feathers'
// lane picked the right card by tag; the rack had no such road and built `GenericCard` for everything.
//
// THE REASON IT COULD NOT was not the selection, it was the CONSTRUCTOR: every custom card took a
// `ParamBridge`, which is Feathers' parameter road, and the rack has a `DeviceHost` and no bridge.
// A card converges when it stops needing the bridge, and this factory only offers the ones that have.
//
// ONE FUNCTION RATHER THAN TWO IF-ELSE CHAINS, deliberately. `source_grid.cpp` states the rule that
// this repo learned the expensive way — *"a second glyph for the same action would read as a second
// action"* — and a second card-selection would be the same failure with more at stake: two hosts
// disagreeing about which UI a device gets is a device that looks like two devices.

namespace feathers::ui {

/// BUILD THE CARD THIS DEVICE ASKS FOR, or `nullptr` if it has no custom one yet.
///
/// NULL IS A REAL ANSWER AND THE CALLER FALLS BACK TO `GenericCard`. A device whose card still needs
/// the parameter bridge is not a device without a UI — it is one whose UI this host cannot build, and
/// answering "generic" here would hide which of the two it is.
/// `mod` and `history` are what a MODULATOR's card needs at construction and an FX card does not.
///
/// **THEY ARE PARAMETERS RATHER THAN A LATER `setModulation` BECAUSE `EnvCard` AND `MsegCard` BUILD
/// THEIR CONTROLS IN THEIR CONSTRUCTORS.** The alternative — making both nullable and re-pointing
/// every control afterwards — buys nothing and adds a null to check at every use. A host that has no
/// modulation state to offer passes null and those two entries DECLINE, so it gets the generic grid
/// rather than a card holding a dangling reference.
///
/// **THE DEVICE BY ITS ID (kill-lanes step 2)**, never a slot or a lane — only the host knows where it
/// keeps the device, and a card that moves with its device needs nothing re-seated.
std::unique_ptr<FxCard> makeCustomCard(backplane::DeviceHost& host, WidgetRegistry& registry,
                                       DeviceInstanceId id, const device::Descriptor& desc,
                                       Notifier* notifier, ModState* mod = nullptr,
                                       EditHistory* history = nullptr);
/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceInstanceId` is a `uint32_t` and a `size_t` would narrow
/// into it without a word, so a host still passing its old address fails to compile here instead.
std::unique_ptr<FxCard> makeCustomCard(backplane::DeviceHost&, WidgetRegistry&, size_t,
                                       const device::Descriptor&, Notifier*, ModState* = nullptr,
                                       EditHistory* = nullptr) = delete;

/// DOES THIS DEVICE HAVE A CUSTOM CARD ON THE SEAM YET? For a host that wants to say so, and for a
/// case that wants to assert the set rather than a count.
bool hasCustomCard(const device::Descriptor& desc) noexcept;

/// ONE DEVICE'S CUSTOM CARD, DECLARED IN THE DEVICE'S OWN HEADER (`plugin/ui/cards/<device>.h`, backlog NY.1)
/// and named by its `device.cmake`; `cmake/FeathersDevices.cmake` assembles the table in registry order,
/// so a new card touches no shared list.
using StripHeightFn = float (*)(size_t outputChipRows);

struct CardReg {
	const char* tag;
	std::unique_ptr<FxCard> (*make)(backplane::DeviceHost&, WidgetRegistry&, DeviceInstanceId, Notifier*,
	                                ModState*, EditHistory*);
	/// Non-null for the modulator strip's cards (`ModPanel`): the card's open height given how many rows its
	/// output chips wrap onto. A device without one is not built by the strip from this table.
	StripHeightFn stripHeight;
};

/// The strip's open height for this device's custom card, or null when the strip does not build one from the
/// table (a generic grid, or the envelope and MSEG which have kinds of their own).
StripHeightFn stripHeightFor(const device::Descriptor& desc) noexcept;

} // namespace feathers::ui
