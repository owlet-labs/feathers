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

#include "plugin/chain_preset.h"

#include "composite/composite_types.h"
#include "rack/fragment.h"
#include "rack/nesting.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_state.h"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace feathers {

namespace {

constexpr size_t kMacro1 = static_cast<size_t>(composite::Param::Macro1);

std::optional<rack::RackPatch> documentOf(std::span<const uint8_t> bytes) {
	rack::RackPatch patch;
	if (bytes.empty()) {
		patch.present = true; // an empty chain
		return patch;
	}
	if (!rack::parseRackText(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &patch)) {
		return std::nullopt;
	}
	patch.present = true;
	return patch;
}

std::vector<uint8_t> bytesOf(const rack::RackPatch& patch) {
	std::string text;
	if (!rack::rackToText(patch, &text, false)) { // compact: a body (`rackToText`)
		return {};
	}
	return { text.begin(), text.end() };
}

/// THE CHAIN'S ROW FOR A CHAIN THAT HAS NONE OF ITS OWN (a rail, the plugin): every control at its default and the macro
/// values `macros` — the values the chain's devices hear.
std::vector<float> chainRow(std::span<const float> macros) {
	const device::Descriptor& desc = chainPresetDescriptor();
	std::vector<float> field(devicePresetRowWidth(desc), 0.0f);
	for (size_t i = 0; i < desc.params.size() && i < field.size(); ++i) {
		field[i] = static_cast<float>(desc.params[i].def);
	}
	for (size_t n = 0; n < composite::kMacroCount && n < macros.size(); ++n) {
		field[kMacro1 + n] = macros[n];
	}
	return field;
}

/// A CHAIN PRESET READ, its tag checked: the row (by key) and the chain's document.
DevicePresetLoad readChainPreset(std::span<const uint8_t> bytes, DevicePresetContents* out, rack::LoadReport* report) {
	if (!readDevicePreset(bytes, out, &chainPresetDescriptor(), report)) {
		return DevicePresetLoad::Malformed;
	}
	if (std::strncmp(out->tag.c_str(), chainPresetDescriptor().traits.tag, device::kTagBytes) != 0) {
		return DevicePresetLoad::OtherDevice;
	}
	return DevicePresetLoad::Loaded;
}

} // namespace

const device::Descriptor& chainPresetDescriptor() noexcept { return rack::kDeviceDescriptor; }
const device::Descriptor& rackPresetDescriptor() noexcept { return rack::parallel::kDescriptor; }

bool isPresetOf(const device::Descriptor& desc, std::span<const uint8_t> bytes) {
	DevicePresetContents contents;
	return readDevicePreset(bytes, &contents)
	       && std::strncmp(contents.tag.c_str(), desc.traits.tag, device::kTagBytes) == 0;
}

bool captureContainerPreset(const backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                            std::vector<uint8_t>* out, size_t* dropped) {
	out->clear();
	std::vector<uint8_t> whole;
	DevicePresetContents contents;
	if (!captureDevicePreset(host, desc, id, &whole) || !readDevicePreset(whole, &contents, &desc)) {
		return false;
	}
	const std::optional<std::vector<uint8_t>> body = rack::withoutOutsideRoutes(contents.body, dropped);
	if (!body.has_value()) {
		return false;
	}
	// AND THE CONTAINER'S OWN SITE (D3 lifted, 2026-10-29), as `captureDevicePreset` already gathered it.
	*out = writeDevicePreset(desc, contents.field, *body, contents.site.has_value() ? &*contents.site : nullptr);
	return !out->empty();
}

bool captureTopPreset(const rack::RackBridge& bridge, std::vector<uint8_t>* out, size_t* dropped) {
	out->clear();
	rack::RackPatch patch = bridge.patch();
	patch.hostSlots.reset(); // a preset carries no slot map (host-slots plan §0.8)
	patch.preset = {};       // nor the name of the preset the rack came from: the browser names the file it reads (backlog QC)
	const std::optional<std::vector<uint8_t>> body = rack::withoutOutsideRoutes(bytesOf(patch), dropped);
	if (!body.has_value() || body->empty()) {
		return false;
	}
	*out = writeDevicePreset(chainPresetDescriptor(), chainRow(patch.macro), *body);
	return !out->empty();
}

DevicePresetLoad applyTopPreset(rack::RackBridge& bridge, std::span<const uint8_t> bytes, rack::LoadReport* report) {
	DevicePresetContents preset;
	if (const DevicePresetLoad read = readChainPreset(bytes, &preset, report); read != DevicePresetLoad::Loaded) {
		return read;
	}
	std::optional<rack::RackPatch> next = documentOf(preset.body);
	if (!next.has_value()) {
		return DevicePresetLoad::Malformed;
	}
	// THE MACRO VALUES ARE THE CHAIN'S ROW (a Chain device's macros are its controls); a row the preset did not carry
	// in full takes the declared default, as every device preset does.
	const device::Descriptor& desc = chainPresetDescriptor();
	next->macro.assign(composite::kMacroCount, 0.0f);
	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		const size_t c = kMacro1 + n;
		next->macro[n] = c < preset.field.size() ? preset.field[c] : static_cast<float>(desc.params[c].def);
	}
	next->exports.clear();
	next->preset = bridge.topPreset(); // the browser names the preset it picked, after the load (backlog QC)
	// THE DEPTH GATE (`fitsNesting`): a chain preset too tall for the chain it is loaded into is refused whole.
	if (!rack::fitsNesting(bridge.nestingDepth(), rack::patchHeight(*next, bridge.catalogue()))) {
		return DevicePresetLoad::TooDeep;
	}
	// THE PLUGIN'S SLOT MAP IS THE SESSION'S, as a device's bypass is: kept, and what no longer resolves frees itself.
	next->hostSlots = bridge.patch().hostSlots;
	const rack::LoadReport refused = bridge.loadPatch(*next);
	if (report != nullptr) {
		report->merge(refused);
	}
	for (size_t n = 0; n < next->macro.size(); ++n) {
		bridge.setMacroByHand(n, next->macro[n]); // the host's cached copy follows, as a hand's turn of the knob
	}
	bridge.markEdited();
	return DevicePresetLoad::Loaded;
}

} // namespace feathers

namespace feathers {

PresetIdentity topPresetOf(const rack::RackBridge& bridge) { return bridge.topPreset(); }

void setTopPresetOf(rack::RackBridge& bridge, const PresetIdentity& identity) { bridge.setTopPreset(identity); }

} // namespace feathers
