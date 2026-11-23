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
#include "plugin/ui/preset_browser.h"
#include "rack/fragment.h"

#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

namespace feathers {
class BridgeDeviceHost;
class ParamBridge;
} // namespace feathers

namespace feathers::ui {

// **A DEVICE'S PRESETS FROM ITS HEADER MENU, THROUGH THE FULL PRESET BOX'S OWN POPUP.** For any device a fragment site holds, this builds
// the `PresetBrowser` a device's window shows (the bank's browser with its views by folder and by tag, and its save dialog with a name AND
// tags), over the same bank the window's browser reads, and opens its popups where the header was pressed. A preset saved here carries its
// tags and is found by them in a full box, and the reverse, because it is the same browser, store and files. Only the full box's furniture
// is absent: no stepper arrows, no name box, no variant slots, since the browser is never drawn.
//
// APPLYING IS ONE UNDO STEP AND ALL OR NOTHING: the device is captured before, `applyDevicePreset` takes the preset whole or not
// at all, and the step restores the captured bytes. Bypass, routes from outside, host slots and the name are left alone,
// as an apply leaves them.
//
// MESSAGE THREAD. The browser lives until the next open, `close()` or the destructor; its popup and dialog may outlive the call.
class DevicePresetMenu {
public:
	DevicePresetMenu(rack::FragmentSite& site, EditHistory& history, Notifier* notifier);
	~DevicePresetMenu();
	DevicePresetMenu(const DevicePresetMenu&) = delete;
	DevicePresetMenu& operator=(const DevicePresetMenu&) = delete;

	/// Whether `id` is a device this site can read and write presets of.
	[[nodiscard]] bool offered(DeviceInstanceId id) const;
	/// The preset the device came from, as the host remembers it (empty: none).
	[[nodiscard]] PresetIdentity current(DeviceInstanceId id) const;
	/// The bank a device's presets live in.
	[[nodiscard]] static std::string bankOf(const device::Descriptor& desc);

	/// THE FULL BOX'S BROWSER FOR DEVICE `id`, read from its bank and wired to capture and apply it (apply is one undo step). With
	/// `host` it stands in that frame at `anchor` (the host's coordinates) so that its popups find the window and open there; without,
	/// it stands alone (a case). Null when the device is not offered. Replaces the browser a previous call made.
	PresetBrowser* prepare(DeviceInstanceId id, visage::Frame* host, const visage::Bounds& anchor);
	/// `prepare`, then the browser's list popup under `anchor`: browse by folder or tag, load, and "Save as..." with its name and tags.
	bool open(DeviceInstanceId id, visage::Frame& host, const visage::Bounds& anchor);
	/// Put the browser away: its dialog goes with it.
	void close();
	[[nodiscard]] PresetBrowser* browser() const noexcept { return browser_.get(); }

private:
	void say(const std::string& text) const;
	/// THE HOST OVER THE SITE'S DEVICES: the site's own (a rack bridge is one), else the synth bridge's adapter, built on first use.
	[[nodiscard]] backplane::DeviceHost* hostOf() const;

	rack::FragmentSite& site_;
	EditHistory& history_;
	Notifier* notifier_;
	mutable std::shared_ptr<BridgeDeviceHost> adapter_;
	std::unique_ptr<PresetProvider> provider_;
	std::unique_ptr<PresetBrowser> browser_;
};

} // namespace feathers::ui
