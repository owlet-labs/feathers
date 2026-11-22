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

#pragma once

#include "backplane/preset_identity.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/notify_tray.h"
#include "rack/fragment.h"

#include <memory>
#include <string>
#include <vector>

namespace feathers {
class BridgeDeviceHost;
class ParamBridge;
} // namespace feathers

namespace feathers::ui {

// **A DEVICE'S PRESETS FROM ITS HEADER MENU, WITHOUT A WIDGET** — list, apply, and save under a name, for any device
// a fragment site holds. The same bank a device's window browser lists (`devicePresetBank`, or the Chains and Racks banks for those two
// containers), read through the same store, so a preset saved here is in the window's list and the reverse.
//
// APPLYING IS ONE UNDO STEP AND ALL OR NOTHING: the device is captured before, `applyDevicePreset` takes the preset whole or not
// at all, and the step restores the captured bytes. Bypass, routes from outside, host slots and the name are left alone,
// as an apply leaves them.
//
// MESSAGE THREAD. Nothing is held between calls but the site, the history and the notifier.
class DevicePresetMenu {
public:
	struct Entry {
		std::string name;
		std::string category; ///< '/'-separated folders under the bank; empty at the top
		bool factory = false; ///< the computed "Init" or a shipped file: never overwritten in one step
		bool operator==(const Entry&) const = default;
	};

	DevicePresetMenu(rack::FragmentSite& site, EditHistory& history, Notifier* notifier)
	    : site_(site), history_(history), notifier_(notifier) {}
	~DevicePresetMenu();
	DevicePresetMenu(const DevicePresetMenu&) = delete;
	DevicePresetMenu& operator=(const DevicePresetMenu&) = delete;

	/// Whether `id` is a device this site can read and write presets of.
	[[nodiscard]] bool offered(DeviceInstanceId id) const;
	/// "Init" first, then the bank's presets by category and name, variants not listed (the browser's own list). Installs the bank's
	/// factory files when missing, as opening a browser does.
	[[nodiscard]] std::vector<Entry> list(DeviceInstanceId id) const;
	/// The preset the device came from, as the host remembers it (empty: none).
	[[nodiscard]] PresetIdentity current(DeviceInstanceId id) const;
	/// Put `entry` into the device as ONE undo step. False, with a word to the user and nothing changed, when it is missing, malformed,
	/// another device's, or refused.
	bool apply(DeviceInstanceId id, const Entry& entry);

	enum class Save {
		Saved,
		/// The name is a user preset already: ask, then call again with `replace`.
		NeedsConfirm,
		/// The name is a factory preset: never written in one step.
		FactoryRefused,
		BadName,
		NotCaptured,
		NoStore,
	};
	/// Save the device as it is now under `typed` ("Name" or "Category/Name"). The device then names that preset.
	Save save(DeviceInstanceId id, const std::string& typed, bool replace = false);
	/// What the prompt opens with: the preset the device came from, as "Category/Name".
	[[nodiscard]] std::string suggestedName(DeviceInstanceId id) const;
	/// The bank a device's presets live in.
	[[nodiscard]] static std::string bankOf(const device::Descriptor& desc);

private:
	void say(const std::string& text) const;
	/// THE HOST OVER THE SITE'S DEVICES: the site's own (a rack bridge is one), else the synth bridge's adapter, built on first use.
	[[nodiscard]] backplane::DeviceHost* hostOf() const;

	rack::FragmentSite& site_;
	EditHistory& history_;
	Notifier* notifier_;
	mutable std::shared_ptr<BridgeDeviceHost> adapter_;
};

} // namespace feathers::ui
