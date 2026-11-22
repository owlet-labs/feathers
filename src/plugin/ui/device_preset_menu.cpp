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

#include "plugin/ui/device_preset_menu.h"

#include "plugin/bridge_device_host.h"
#include "plugin/chain_preset.h"
#include "plugin/device_preset.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "plugin/ui/preset_browser.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace feathers::ui {

namespace {

/// THE KIND A DEVICE'S PRESETS ARE OF: the Chain and the Rack are the two containers whose presets have their own banks and are read
/// as `chain_preset.h` reads them; every other device is its own descriptor.
const device::Descriptor& kindOf(const device::Descriptor& desc) {
	if (device::sameTag(desc.traits.tag, chainPresetDescriptor().traits.tag)) {
		return chainPresetDescriptor();
	}
	if (device::sameTag(desc.traits.tag, rackPresetDescriptor().traits.tag)) {
		return rackPresetDescriptor();
	}
	return desc;
}

bool isContainerKind(const device::Descriptor& desc) {
	return &kindOf(desc) != &desc || &desc == &chainPresetDescriptor() || &desc == &rackPresetDescriptor();
}

/// THE MORE SPECIFIC WORDS A REFUSED LOAD GETS (`DevicePresetProvider`'s, `container_presets.cpp`'s).
const char* refusalWords(DevicePresetLoad load) {
	switch (load) {
	case DevicePresetLoad::Loaded:
		return "";
	case DevicePresetLoad::Malformed:
		return "Preset did not load: the file is not a preset of this device";
	case DevicePresetLoad::OtherDevice:
		return "Preset did not load: it was saved from a different device";
	case DevicePresetLoad::BodyRefused:
		return "Preset did not load: its chains were refused";
	case DevicePresetLoad::TooDeep:
		return rack::kNestingFullMessage;
	}
	return "";
}

bool isFactoryEntry(const std::string& name, const std::string& category, const std::filesystem::path& path) {
	if (category.empty() && name == "Init") {
		return true;
	}
	if (path.empty()) {
		return false;
	}
	const std::filesystem::path relative = path.lexically_relative(PresetStore::root());
	const auto files = factoryPresetSource();
	return std::any_of(files.begin(), files.end(), [&relative](const FactoryPresetFile& f) {
		return std::filesystem::path(std::string(f.path)) == relative;
	});
}

int64_t nowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/// THE HOST OVER A SITE'S DEVICES: the site's own (a rack bridge is one), else the synth bridge's adapter, built on first use.
backplane::DeviceHost* hostFor(rack::FragmentSite& site, std::shared_ptr<BridgeDeviceHost>& adapter) {
	if (backplane::DeviceHost* own = site.fragmentHost()) {
		return own;
	}
	// THE SYNTH'S FX LANE AND MODULATOR STRIP: sites over the parameter bridge, whose device host is the adapter.
	ParamBridge* bridge = dynamic_cast<ParamBridge*>(&site);
	if (bridge == nullptr) {
		if (auto* strip = dynamic_cast<ParamBridge::StripSite*>(&site)) {
			bridge = &strip->bridge();
		}
	}
	if (bridge == nullptr) {
		return nullptr;
	}
	if (adapter == nullptr) {
		adapter = std::make_shared<BridgeDeviceHost>(*bridge);
	}
	return adapter.get();
}

/// ONE APPLY, AS A STEP: undo puts the captured bytes back (and the name the device came from), redo applies the preset again.
class PresetLoadEdit final : public EditRecord {
public:
	PresetLoadEdit(rack::FragmentSite& site, DeviceInstanceId id, const device::Descriptor& desc, std::vector<uint8_t> before,
	               std::vector<uint8_t> after, PresetIdentity was, PresetIdentity now,
	               std::shared_ptr<BridgeDeviceHost> adapter)
	    : site_(site), life_(site.fragmentLife()), adapter_(std::move(adapter)), id_(id), desc_(desc), before_(std::move(before)), after_(std::move(after)),
	      was_(std::move(was)), now_(std::move(now)) {}
	void undo() const override { put(before_, was_); }
	void redo() const override { put(after_, now_); }
	const char* verb() const override { return "Preset"; }

private:
	void put(const std::vector<uint8_t>& bytes, const PresetIdentity& identity) const {
		backplane::DeviceHost* host = life_.expired() ? nullptr : hostFor(site_, adapter_);
		if (host == nullptr || host->descriptorOf(id_) != &desc_) {
			return;
		}
		if (applyDevicePreset(*host, kindOf(desc_), id_, bytes) == DevicePresetLoad::Loaded) {
			host->setDevicePreset(id_, identity);
		}
	}

	rack::FragmentSite& site_;
	std::weak_ptr<const void> life_;
	mutable std::shared_ptr<BridgeDeviceHost> adapter_;
	DeviceInstanceId id_;
	const device::Descriptor& desc_;
	std::vector<uint8_t> before_;
	std::vector<uint8_t> after_;
	PresetIdentity was_;
	PresetIdentity now_;
};

} // namespace

DevicePresetMenu::~DevicePresetMenu() = default;

std::string DevicePresetMenu::bankOf(const device::Descriptor& desc) {
	const device::Descriptor& kind = kindOf(desc);
	if (&kind == &chainPresetDescriptor()) {
		return kChainPresetBank;
	}
	if (&kind == &rackPresetDescriptor()) {
		return kRackPresetBank;
	}
	return devicePresetBank(kind);
}

backplane::DeviceHost* DevicePresetMenu::hostOf() const { return hostFor(site_, adapter_); }

void DevicePresetMenu::say(const std::string& text) const {
	if (notifier_ != nullptr) {
		notifier_->say(text);
	}
}

bool DevicePresetMenu::offered(DeviceInstanceId id) const {
	const backplane::DeviceHost* host = hostOf();
	return host != nullptr && host->descriptorOf(id) != nullptr && !PresetStore::root().empty();
}

std::vector<DevicePresetMenu::Entry> DevicePresetMenu::list(DeviceInstanceId id) const {
	std::vector<Entry> out;
	const backplane::DeviceHost* host = hostOf();
	const device::Descriptor* desc = host != nullptr ? host->descriptorOf(id) : nullptr;
	if (desc == nullptr) {
		return out;
	}
	const std::string bank = bankOf(*desc);
	(void)PresetStore::installFactoryFiles(bank);
	out.push_back({ "Init", {}, true });
	const bool container = isContainerKind(*desc);
	for (const StoredPreset& stored : PresetStore::loadAll(bank)) {
		if (stored.category.empty() && stored.name == "Init") {
			continue; // the computed one, listed first
		}
		if (PresetBrowser::isVariantFile(stored.name) || (container && !isPresetOf(kindOf(*desc), stored.bytes))) {
			continue;
		}
		out.push_back({ stored.name, stored.category, isFactoryEntry(stored.name, stored.category, stored.path) });
	}
	return out;
}

PresetIdentity DevicePresetMenu::current(DeviceInstanceId id) const {
	const backplane::DeviceHost* host = hostOf();
	return host != nullptr ? host->devicePreset(id) : PresetIdentity {};
}

std::string DevicePresetMenu::suggestedName(DeviceInstanceId id) const {
	const PresetIdentity now = current(id);
	return now.category.empty() ? now.name : now.category + "/" + now.name;
}

bool DevicePresetMenu::apply(DeviceInstanceId id, const Entry& entry) {
	backplane::DeviceHost* host = hostOf();
	const device::Descriptor* desc = host != nullptr ? host->descriptorOf(id) : nullptr;
	if (desc == nullptr) {
		return false;
	}
	std::vector<uint8_t> bytes;
	bool found = false;
	for (StoredPreset& stored : PresetStore::loadAll(bankOf(*desc))) {
		if (stored.name == entry.name && stored.category == entry.category) {
			bytes = std::move(stored.bytes);
			found = true;
			break;
		}
	}
	if (!found && entry.category.empty() && entry.name == "Init") {
		bytes = initDevicePreset(kindOf(*desc));
		found = !bytes.empty();
	}
	if (!found) {
		say("Preset not found: " + entry.name);
		return false;
	}
	std::vector<uint8_t> before;
	if (!captureDevicePreset(*host, kindOf(*desc), id, &before)) {
		say("Preset not loaded: the device could not be read");
		return false;
	}
	const PresetIdentity was = host->devicePreset(id);
	const DevicePresetLoad load = applyDevicePreset(*host, kindOf(*desc), id, bytes);
	if (load != DevicePresetLoad::Loaded) {
		say(refusalWords(load));
		return false;
	}
	const PresetIdentity now { entry.name, entry.category };
	host->setDevicePreset(id, now);
	history_.record(std::make_unique<PresetLoadEdit>(site_, id, *desc, std::move(before), std::move(bytes), was, now, adapter_));
	say("Preset: " + entry.name);
	return true;
}

DevicePresetMenu::Save DevicePresetMenu::save(DeviceInstanceId id, const std::string& typed, bool replace) {
	backplane::DeviceHost* host = hostOf();
	const device::Descriptor* desc = host != nullptr ? host->descriptorOf(id) : nullptr;
	if (desc == nullptr || PresetStore::root().empty()) {
		return Save::NoStore;
	}
	const auto [name, category] = PresetStore::splitTyped(typed);
	if (name.empty()) {
		say("Preset not saved: give it a name");
		return Save::BadName;
	}
	const std::string bank = bankOf(*desc);
	const std::filesystem::path path = PresetStore::pathFor(name, bank, category);
	if (path.empty()) {
		return Save::BadName;
	}
	std::error_code ec;
	const bool exists = std::filesystem::exists(path, ec);
	if (isFactoryEntry(name, category, path) && (exists || (category.empty() && name == "Init"))) {
		say("\"" + name + "\" is a factory preset: save under another name");
		return Save::FactoryRefused;
	}
	if (exists && !replace) {
		return Save::NeedsConfirm;
	}
	std::vector<uint8_t> bytes;
	const bool captured = isContainerKind(*desc) ? captureContainerPreset(*host, kindOf(*desc), id, &bytes) : captureDevicePreset(*host, *desc, id, &bytes);
	if (!captured || bytes.empty()) {
		say("Preset not saved: the device could not be read");
		return Save::NotCaptured;
	}
	PresetMeta meta;
	meta.saved = nowSeconds();
	meta.category = category;
	if (!PresetStore::save(name, bytes, bank, category, meta)) {
		say("Preset not saved: the file could not be written");
		return Save::NoStore;
	}
	host->setDevicePreset(id, PresetIdentity { name, PresetStore::sanitiseCategory(category) });
	say("Preset saved: " + name);
	return Save::Saved;
}

} // namespace feathers::ui
