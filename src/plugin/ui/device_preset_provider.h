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
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/preset_browser.h"
#include "rack/rack_device.h"

#include <array>
#include <string>

// **ONE DEVICE, AS A PRESET BROWSER'S PROVIDER** (backlog FO). The browser knows entries; this knows
// that an entry is `device_preset.h`'s bytes for one device in one host. Any device panel that wants
// presets holds one of these and hands it to a `PresetBrowser` over `devicePresetBank(desc)` — nothing
// here names SKLITTER. When the keyed state format (backlog JF) lands, `device_preset.cpp` is what
// changes and neither this nor the browser does.

namespace feathers::ui {

class DevicePresetProvider final : public PresetProvider {
public:
	/// `keep`: the controls a load leaves as they were, beyond the bypass (copied, up to eight).
	/// `notifier` may be null; a refusal is then silent, which only a case should want.
	DevicePresetProvider(backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
	                     std::span<const uint8_t> keep, Notifier* notifier)
	    : host_(host), desc_(desc), id_(id), notifier_(notifier) {
		keepCount_ = std::min(keep.size(), keep_.size());
		std::copy_n(keep.begin(), keepCount_, keep_.begin());
	}

	bool capture(PresetData& out) override {
		if (captureDevicePreset(host_, desc_, id_, &out.bytes)) {
			return true;
		}
		say("Preset not saved — the device's chains could not be read");
		return false;
	}

	bool apply(const PresetData& data) override {
		last_ = applyDevicePreset(host_, desc_, id_, data.bytes, std::span<const uint8_t>(keep_.data(), keepCount_));
		// AND IF IT REFUSES, SAY SO — the editor's own rule for its presets: a name clicked and a sound that
		// did not change, with nothing said, is the failure a player cannot diagnose.
		const std::string device = desc_.traits.name != nullptr ? desc_.traits.name : "device";
		switch (last_) {
		case DevicePresetLoad::Loaded:
			break;
		case DevicePresetLoad::Malformed:
			say("Preset did not load — the file is not a " + device + " preset");
			break;
		case DevicePresetLoad::OtherDevice:
			say("Preset did not load — it was saved from a different device");
			break;
		case DevicePresetLoad::BodyRefused:
			say("Preset did not load — its chains were refused");
			break;
		case DevicePresetLoad::TooDeep:
			say(rack::kNestingFullMessage); // the depth gate: its site would stand deeper than any file gives back
			break;
		}
		return last_ == DevicePresetLoad::Loaded;
	}

	[[nodiscard]] DevicePresetLoad lastLoad() const noexcept { return last_; }

private:
	void say(const std::string& text) {
		if (notifier_ != nullptr) {
			notifier_->say(text);
		}
	}

	backplane::DeviceHost& host_;
	const device::Descriptor& desc_;
	DeviceInstanceId id_;
	Notifier* notifier_;
	std::array<uint8_t, 8> keep_ {};
	size_t keepCount_ = 0;
	DevicePresetLoad last_ = DevicePresetLoad::Loaded;
};

} // namespace feathers::ui
