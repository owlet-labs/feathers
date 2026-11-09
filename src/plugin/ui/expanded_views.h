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

#include "plugin/ui/widget_registry.h"
#include "device/registry.h"
#include "backplane/device_host.h"
#include "plugin/ui/expanded_overlay.h"

#include <cstddef>
#include <memory>

// WHERE `Traits::hasExpanded` IS FINALLY READ.
//
// IT WAS DECLARED AND READ BY NOTHING (backlog 16, "half of device::Traits is inert"), which is the
// state a flag reaches when it is added ahead of its consumer. The dual-view brief made one demand
// about it and this file is that demand honoured: **the flag gets a reader in the same change that
// flips a device to true**, because a fifth inert flag is the one outcome that work must not produce.
//
// AND THE FLAG IS NOT A CONVENIENCE HERE — it is the whole dispatch. A device declaring
// `hasExpanded = false` has no expanded view, so the affordance that opens one is not drawn for it;
// a device declaring `true` and having no case below is a device this build cannot open, which the
// nullptr says honestly rather than by drawing an empty panel.

namespace feathers {
class ParamBridge;
}

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

/// BUILD THE EXPANDED VIEW FOR THE DEVICE `id`, or nullptr when there is none.
///
/// Nullptr for two different reasons and the caller does not need to tell them apart: the device
/// declares no expanded view, or it declares one this build cannot construct. Either way there is
/// nothing to open, and drawing a way in would be an affordance that does nothing.
/// BUILD A DEVICE'S EXPANDED VIEW, for whichever host is asking.
///
/// **THE BRIDGE IS OPTIONAL AND `nullptr` IS THE RACK.** Panels converge onto `backplane::DeviceHost`
/// one device at a time; until a panel has, it needs Feathers' parameter road and cannot be built by a
/// host that has none. An entry that still needs the bridge answers `nullptr` when it is absent —
/// which is the same honest answer `makeCustomCard` gives, and it says "this host cannot build it"
/// rather than "this device has no panel".
///
/// ONE TABLE, NOT TWO. A second factory for the converged devices would be two selections that could
/// disagree about which panel a device gets, and this repo has already paid once for a second copy of
/// a shared answer.
std::unique_ptr<ExpandedView> makeExpandedView(backplane::DeviceHost& host, ParamBridge* bridge,
                                               WidgetRegistry& registry,
                                               DeviceInstanceId id, const device::Descriptor& desc,
                                               Notifier* notifier = nullptr);
/// **A LANE OR A SLOT IS NOT AN ID** — see `makeCustomCard`'s deleted twin.
std::unique_ptr<ExpandedView> makeExpandedView(backplane::DeviceHost&, ParamBridge*, WidgetRegistry&,
                                               size_t, const device::Descriptor&,
                                               Notifier* = nullptr) = delete;

/// WHETHER ANYTHING WOULD BE BUILT — asked by the head that draws the way in, so a button is drawn
/// exactly when pressing it would do something. The honesty rule, in the form it takes here.
///
/// IT IS NOT `traits.hasExpanded` SPELLED DIFFERENTLY, and the difference is the point: this answers
/// for what the build can CONSTRUCT, which is the same structural-else the fx lane uses to choose a
/// card. A device could declare the flag before its view exists, and the button must not appear a
/// moment before the panel does.
bool hasExpandedView(const device::Descriptor& desc);

/// **A MODULE ABOVE THIS LIBRARY REGISTERS ITS VIEW HERE.** The rack's chain editor lives in
/// feathers-rack-editor, which links feathers-ui — so this file cannot name it, and the registration is a
/// hook rather than an include. Registered views are consulted before the table above. Idempotent per tag.
using ExpandedMaker = std::unique_ptr<ExpandedView> (*)(backplane::DeviceHost&, ParamBridge*, WidgetRegistry&,
                                                       DeviceInstanceId, Notifier*);
void registerExpandedView(const char* tag, ExpandedMaker maker);

/// **THE DESCRIPTOR A DEVICE'S PRE/POST PANEL IS OPENED WITH** (backlog NR) — tag `site`, no controls, a panel and nothing
/// else. A host opens it through `makeExpandedView` with the site's key (`siteKeyOf(device)`) as the id; the rack's editor
/// module registers the view (`rack::registerSiteExpandedView`), because this library cannot name it.
const device::Descriptor& siteViewDescriptor();

} // namespace feathers::ui
