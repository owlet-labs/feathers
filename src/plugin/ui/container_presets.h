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
#include "plugin/chain_preset.h"
#include "plugin/ui/preset_browser.h"

#include <cstdint>
#include <functional>
#include <memory>

// **THE THREE CHAIN AND RACK PRESET BROWSERS** (backlog OU): the shared
// `PresetBrowser` at a panel's size over one bank, listing only that bank's kind, with the provider that knows where
// a load lands. A Chain's window and a Rack's window show theirs in the title row, the rack plugin beside the settings
// door. A Rack's rail has none: a Chain added to it brings its own. A refusal is said on `notifier`, in the device presets' words.

namespace feathers::rack {
class RackBridge;
}

namespace feathers::ui {

class Notifier;

/// A browser and its provider, built together. The provider is declared first, so the browser goes first.
struct ContainerPresets {
	std::unique_ptr<PresetProvider> provider;
	std::unique_ptr<PresetBrowser> browser;
};

/// A Chain device's presets (`Chains/`): a load replaces its chain and its row, the bypass kept.
ContainerPresets makeChainDevicePresets(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier);
/// A Rack device's presets (`Racks/`): a load replaces the whole rack, mode and chains.
ContainerPresets makeRackDevicePresets(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier);
/// **THE RACK PLUGIN'S WHOLE STATE** (`Chains/`). `loaded` runs after a load took (the editor's cards follow the record).
ContainerPresets makeTopPresets(rack::RackBridge& bridge, Notifier* notifier, std::function<void()> loaded = {});

} // namespace feathers::ui
