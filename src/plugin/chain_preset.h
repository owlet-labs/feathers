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

#include "plugin/device_preset.h"
#include "plugin/preset_store.h"
#include "backplane/preset_identity.h"

#include <cstdint>
#include <span>
#include <vector>

// **PRESETS FOR CHAINS AND RACKS, AND FOR THE RACK PLUGIN** — backlog OU.
//
// TWO KINDS, EACH A DEVICE PRESET THAT ALREADY EXISTS (`device_preset.h`): a CHAIN preset is the Chain device's (tag
// `cmps`: its row and its chain's document), a RACK preset is the Rack device's (its row, mode included, and its `RKP1`
// body of chains). Each lives in its own bank and a browser opens one bank, so a browser lists only its own kind; the
// tag is checked on load, so a chain preset is refused by a Rack (`OtherDevice`) and never turns it into a Chain.
//
// **WHAT A CAPTURE DROPS**: every route from outside the saved container — the
// `Outer` routes and the export table at its own level (`rack::withoutOutsideRoutes`). Sites of the devices inside
// travel with them (they are in the chain's document), and so does the container's OWN site (D3 lifted),
// as every device preset now carries its site (`device_preset.h`).
//
// A CHAIN PRESET LANDS IN TWO PLACES, the same bytes: a Chain device (`applyDevicePreset`) and the rack plugin's top
// level (`applyTopPreset`, the plugin IS a chain and eight macros). A Rack's rail has NO road of its own (2026-10-29: add a chain and use its browser): a Chain device added to that chain takes the preset.
// MESSAGE THREAD, every function here.

namespace feathers {

namespace rack {
class RackBridge;
struct LoadReport;
} // namespace rack

/// THE TWO BANKS, beside `Feathers/` and `Devices/` under the preset root (`preset_store.h`).
inline constexpr const char* kChainPresetBank = "Chains";
inline constexpr const char* kRackPresetBank = "Racks";

/// The Chain device's and the Rack device's descriptors — the two kinds.
[[nodiscard]] const device::Descriptor& chainPresetDescriptor() noexcept;
[[nodiscard]] const device::Descriptor& rackPresetDescriptor() noexcept;

/// **WHICH KIND THESE BYTES ARE** — a device preset whose tag is `desc`'s. What a browser lists by (`PresetBrowser::accepts`).
[[nodiscard]] bool isPresetOf(const device::Descriptor& desc, std::span<const uint8_t> bytes);

/// **A CHAIN OR RACK DEVICE AS PRESET BYTES** — `captureDevicePreset` with the body's outside routes dropped. `desc` is
/// one of the two above. `dropped` (if given) adds how many routes and export rows went.
bool captureContainerPreset(const backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                            std::vector<uint8_t>* out, size_t* dropped = nullptr);


/// **THE RACK PLUGIN'S WHOLE STATE AS A CHAIN PRESET** — its chain with outside routes dropped (none reach it) and no
/// host slots, under the Chain's row with the plugin's macro values.
bool captureTopPreset(const rack::RackBridge& bridge, std::vector<uint8_t>* out, size_t* dropped = nullptr);
/// **WHICH PRESET THE RACK PLUGIN'S BROWSER CAME FROM** (backlog QC) — `RackBridge::topPreset`, reached through here so a
/// browser over the bridge needs only this header.
PresetIdentity topPresetOf(const rack::RackBridge& bridge);
void setTopPresetOf(rack::RackBridge& bridge, const PresetIdentity& identity);

/// **A CHAIN PRESET AS THE RACK PLUGIN'S WHOLE STATE** — devices, routes and macros replaced; the plugin's host slot
/// pool kept (the slots that no longer resolve free at the next visit). The macro values go to the host as values a
/// hand set (`setMacroByHand`), and the patch is marked edited: a preset load is a hand's act, not a project opening.
DevicePresetLoad applyTopPreset(rack::RackBridge& bridge, std::span<const uint8_t> bytes,
                                rack::LoadReport* report = nullptr);

} // namespace feathers
