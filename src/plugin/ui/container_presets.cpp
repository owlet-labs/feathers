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

#include "plugin/ui/container_presets.h"

#include "plugin/ui/notify_tray.h"
#include "rack/rack_device.h"

#include <string>
#include <utility>

namespace feathers::ui {

namespace {

void say(Notifier* notifier, const std::string& text) {
	if (notifier != nullptr) {
		notifier->say(text);
	}
}

/// THE DEVICE PRESETS' WORDS (`DevicePresetProvider`): a name clicked and a sound that did not change, with nothing
/// said, is the failure a player cannot diagnose.
bool said(Notifier* notifier, DevicePresetLoad load, const char* kind) {
	switch (load) {
	case DevicePresetLoad::Loaded:
		return true;
	case DevicePresetLoad::Malformed:
		say(notifier, std::string("Preset did not load — the file is not a ") + kind + " preset");
		break;
	case DevicePresetLoad::OtherDevice:
		say(notifier, std::string("Preset did not load — it is not a ") + kind + " preset");
		break;
	case DevicePresetLoad::BodyRefused:
		say(notifier, "Preset did not load — its chains were refused");
		break;
	case DevicePresetLoad::TooDeep:
		say(notifier, rack::kNestingFullMessage);
		break;
	}
	return false;
}

bool captured(Notifier* notifier, bool ok) {
	if (!ok) {
		say(notifier, "Preset not saved — the chains could not be read");
	}
	return ok;
}

ContainerPresets browserOver(const device::Descriptor& kind, const char* bank, std::unique_ptr<PresetProvider> provider,
                             Notifier* notifier) {
	ContainerPresets out;
	out.provider = std::move(provider);
	out.browser = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                              std::vector<PresetData> { PresetData { initDevicePreset(kind) } },
	                                              bank, PresetBrowser::kPanelStyle);
	out.browser->accepts = [&kind](std::span<const uint8_t> bytes) { return isPresetOf(kind, bytes); };
	out.browser->setBounds(0.0f, 0.0f, out.browser->barWidth(), PresetBrowser::kPanelStyle.height);
	out.browser->loadBankFromDisk();
	out.browser->setProvider(out.provider.get());
	out.browser->setNotifier(notifier);
	return out;
}

ContainerPresets devicePresets(const device::Descriptor& kind, const char* bank, const char* name,
                               backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier) {
	auto provider = std::make_unique<FunctionPresetProvider>(
	    [&host, &kind, id, notifier](PresetData& out) {
		    return captured(notifier, captureContainerPreset(host, kind, id, &out.bytes));
	    },
	    [&host, &kind, id, notifier, name](const PresetData& data) {
		    return said(notifier, applyDevicePreset(host, kind, id, data.bytes), name);
	    });
	ContainerPresets out = browserOver(kind, bank, std::move(provider), notifier);
	// THE PANEL OPENS ON THE PRESET THIS DEVICE CAME FROM, and names every later one into the patch (backlog QC).
	bindPresetIdentity(
	    *out.browser, [&host, id] { return host.devicePreset(id); },
	    [&host, id](const PresetIdentity& identity) { host.setDevicePreset(id, identity); });
	return out;
}

} // namespace

ContainerPresets makeChainDevicePresets(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier) {
	return devicePresets(chainPresetDescriptor(), kChainPresetBank, "chain", host, id, notifier);
}

ContainerPresets makeRackDevicePresets(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier) {
	return devicePresets(rackPresetDescriptor(), kRackPresetBank, "rack", host, id, notifier);
}

ContainerPresets makeTopPresets(rack::RackBridge& bridge, Notifier* notifier, std::function<void()> loaded) {
	auto provider = std::make_unique<FunctionPresetProvider>(
	    [&bridge, notifier](PresetData& out) { return captured(notifier, captureTopPreset(bridge, &out.bytes)); },
	    [&bridge, notifier, loaded = std::move(loaded)](const PresetData& data) {
		    if (!said(notifier, applyTopPreset(bridge, data.bytes), "chain")) {
			    return false;
		    }
		    if (loaded) {
			    loaded();
		    }
		    return true;
	    });
	ContainerPresets out = browserOver(chainPresetDescriptor(), kChainPresetBank, std::move(provider), notifier);
	// THE RACK PLUGIN'S BROWSER OPENS ON THE PRESET ITS PATCH CAME FROM (backlog QC): the bridge's `topPreset`, saved at the
	// patch's root.
	bindPresetIdentity(
	    *out.browser, [&bridge] { return topPresetOf(bridge); },
	    [&bridge](const PresetIdentity& identity) { setTopPresetOf(bridge, identity); });
	return out;
}

} // namespace feathers::ui
