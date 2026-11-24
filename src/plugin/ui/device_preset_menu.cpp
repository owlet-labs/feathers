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

DevicePresetMenu::DevicePresetMenu(rack::FragmentSite& site, EditHistory& history, Notifier* notifier)
    : site_(site), history_(history), notifier_(notifier) {}

void DevicePresetMenu::close() {
	if (browser_ != nullptr) {
		browser_->setProvider(nullptr);
		if (browser_->parent() != nullptr) {
			browser_->parent()->removeChild(browser_.get()); // a parent torn down first has already let go of it
		}
	}
	browser_.reset();
	provider_.reset();
}

PresetIdentity DevicePresetMenu::current(DeviceInstanceId id) const {
	const backplane::DeviceHost* host = hostOf();
	return host != nullptr ? host->devicePreset(id) : PresetIdentity {};
}

PresetBrowser* DevicePresetMenu::prepare(DeviceInstanceId id, visage::Frame* frame, const visage::Bounds& anchor) {
	backplane::DeviceHost* host = hostOf();
	const device::Descriptor* desc = host != nullptr ? host->descriptorOf(id) : nullptr;
	if (desc == nullptr || PresetStore::root().empty()) {
		return nullptr;
	}
	close();
	const device::Descriptor& kind = kindOf(*desc);
	const bool container = isContainerKind(*desc);
	// THE PROVIDER: capture as the window's browser does, and apply as ONE undo step. The step is recorded when the browser reports the pick,
	// by which time the browser has told the host which preset the device now came from.
	struct Pending {
		std::vector<uint8_t> before;
		std::vector<uint8_t> after;
		PresetIdentity was;
		bool open = false;
	};
	auto pending = std::make_shared<Pending>();
	provider_ = std::make_unique<FunctionPresetProvider>(
	    [this, host, desc, id, &kind, container](PresetData& out) {
		    const bool ok = container ? captureContainerPreset(*host, kind, id, &out.bytes) : captureDevicePreset(*host, *desc, id, &out.bytes);
		    if (!ok) {
			    say("Preset not saved: the device could not be read");
		    }
		    return ok;
	    },
	    [this, host, id, &kind, pending](const PresetData& data) {
		    std::vector<uint8_t> before;
		    if (!captureDevicePreset(*host, kind, id, &before)) {
			    say("Preset not loaded: the device could not be read");
			    return false;
		    }
		    const PresetIdentity was = host->devicePreset(id);
		    const DevicePresetLoad load = applyDevicePreset(*host, kind, id, data.bytes);
		    if (load != DevicePresetLoad::Loaded) {
			    say(refusalWords(load));
			    return false;
		    }
		    *pending = Pending { std::move(before), data.bytes, was, true };
		    return true;
	    });
	browser_ = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { initDevicePreset(kind) } },
	                                           bankOf(*desc), PresetBrowser::kPanelStyle);
	if (container) {
		browser_->accepts = [&kind](std::span<const uint8_t> bytes) { return isPresetOf(kind, bytes); };
	}
	// THE BROWSER STANDS WHERE THE HEADER WAS PRESSED: its name box (what the list opens under, one arrow in from its left edge) at the
	// anchor, and its dialog under its bar. Never drawn; hidden once placed.
	const float width = browser_->barWidth();
	browser_->setBounds(anchor.x() - PresetBrowser::kPanelStyle.arrowWidth, anchor.y(), width, PresetBrowser::barHeightFor(PresetBrowser::kPanelStyle));
	browser_->loadBankFromDisk();
	browser_->setProvider(provider_.get());
	browser_->setNotifier(notifier_);
	bindPresetIdentity(
	    *browser_, [host, id] { return host->devicePreset(id); }, [host, id](const PresetIdentity& identity) { host->setDevicePreset(id, identity); });
	const std::weak_ptr<const void> life = site_.fragmentLife();
	browser_->onSelected([this, host, desc, id, pending, life](int) {
		if (!pending->open || life.expired()) {
			return;
		}
		pending->open = false;
		const PresetIdentity now = host->devicePreset(id);
		history_.record(std::make_unique<PresetLoadEdit>(site_, id, *desc, std::move(pending->before), std::move(pending->after), pending->was, now, adapter_));
		say(now.name.empty() ? std::string("Preset loaded") : "Preset: " + now.name);
	});
	if (frame != nullptr) {
		frame->addChild(browser_.get());
		browser_->setVisible(false);
	}
	return browser_.get();
}

bool DevicePresetMenu::open(DeviceInstanceId id, visage::Frame& host, const visage::Bounds& anchor) {
	PresetBrowser* browser = prepare(id, &host, anchor);
	if (browser == nullptr) {
		return false;
	}
	browser->showPresetMenu();
	return true;
}

} // namespace feathers::ui
