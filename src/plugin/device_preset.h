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
#include "device/registry.h"
#include "rack/rack_state.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// **ONE DEVICE'S PRESET — backlog FO, and the answer to its first question ("whose feature is it?"):
// every device's.** Nothing below names SKLITTER. It reads a device through the `DeviceHost` every
// surface already edits it through, so the same two calls serve a device in Feathers' fx lane, in the
// rack, and in a chain inside either — whichever host the panel was handed.
//
// **THE BYTES ARE A RACK PATCH HOLDING ONE DEVICE**, written by `rack::writeRack` and read by
// `rack::parseRack`. Not a second format: a device's row and its container body already have a codec,
// the one every patch and every chain body is written in, and a preset is exactly "this device, as a
// patch would hold it". It also means the file is a valid rack patch — which is free, and which the
// folder-tree proposal leans on.
//
// **WHAT IT HOLDS: the row (every control, and a device's extra state) and the container body** — for
// SKLITTER, both lanes' devices, settings and inner routes. **WHAT IT DOES NOT**: anything the running
// instance holds that is not state — SKLITTER's captured audio ring is audio-thread memory in the
// instance and appears in neither the row nor the body, by construction rather than by exclusion.
//
// **WHAT A LOAD LEAVES ALONE: the bypass (`Traits::enableControl`) always, and whatever the caller
// names in `keep`.** A preset is a sound; whether the device is switched in is the session's, and a
// preset that un-bypassed a device would change the mix by being browsed. SKLITTER adds `Engage`,
// which is a GESTURE held by the host's automation (`engage.h`) — browsing presets must not start the
// glitch. Both are still WRITTEN, so a later decision can honour them without a format change.
//
// **CONTROLS ARE STORED BY KEY** (backlog JF step 1c, 2026-09-25). They were stored by position, gated
// only by the rack format's version, so a control inserted into a device made every older preset load
// into the wrong controls while reporting `Loaded` (the hazard accepted on 2026-09-24 until this
// landed). Now each value is found by `ParamDesc::key` wherever the control stands, a new control takes
// its default, a Choice is found by its label, and what did not load as saved is a load report. Presets
// saved in the old binary encoding are refused (`Malformed`): a clean break.
//
// **AND ITS SITE — its PRE and POST chains, as a Rack or a Chain preset carries its inner chains.**
// The site's bytes go in the patch's own `site` field, the one a full patch already writes, and the routes from PRE to
// the device's OWN controls (`Source::exported(device, row)`, held by the HOST's record, not the site's) go in the
// patch's route list as `Export` routes from the preset's device to itself. POST's reads of PRE travel inside the site
// bytes, re-pointed at the preset's device. Every other route across the edge is DROPPED, as OU drops them: from a PRE
// export to a later device, and from outside into PRE or POST. **ON LOAD a preset with a site REPLACES the device's
// site whole** (opening one if it had none) and its self-routes; **a preset with no site leaves the device's site and
// those routes alone** — a site is treated like the bypass rather than like a control at its default, so a preset
// saved before sites travelled does not wipe someone's chains (the integrator's default, put to).
//
// MESSAGE THREAD, both directions. A load writes fields through `setField` and the body through
// `ContainerOps::applyState`, which is the record-only road: no engine is touched and nothing is
// allocated on the audio thread (the inner engine converges at its own drain).

namespace feathers {

/// **THE DEVICE AS IT IS NOW, AS PRESET BYTES.** False — `out` empty — if the container could not say
/// its body. The body comes from the running instance when the host files one (`containerSurfaceOf`: a
/// chain edited through its own editor is current only there) and from the host's stored bytes
/// otherwise, the order `rack::patchOfRecord` asks in.
bool captureDevicePreset(const backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                         std::vector<uint8_t>* out);

/// **A DEVICE'S SITE AS A PRESET HOLDS IT** — the `SIT1` bytes, POST's reads of PRE re-pointed at the preset's own
/// device, and PRE's routes into that device's own controls, by the control's KEY.
struct DevicePresetSite {
	std::vector<uint8_t> body;
	struct Route {
		/// The export row WITH its scope bit, as `backplane::Source::exported` holds it.
		uint32_t index = 0;
		std::string key;
		float amount = 0.0f;
		device::Transfer transfer = device::Transfer::Lin;
	};
	std::vector<Route> routes;
};

/// **PRESET BYTES FROM A ROW AND A BODY** — what `captureDevicePreset` writes once it has read them, for a caller
/// that has the row and the body from somewhere other than a host (backlog OU: a Rack's chain, the rack plugin's
/// own state). `field` is the controls then the extra state; `devicePresetRowWidth` is its full width. Empty for a
/// patch that cannot be named in a file.
std::vector<uint8_t> writeDevicePreset(const device::Descriptor& desc, const std::vector<float>& field,
                                       std::vector<uint8_t> body, const DevicePresetSite* site = nullptr);
size_t devicePresetRowWidth(const device::Descriptor& desc);

/// **"INIT", HONESTLY** — every control at its declared default, extra state at zero, and an EMPTY body
/// (for a container: every chain emptied). The device's own default state, the same thing Feathers'
/// "Init" means for the plugin.
std::vector<uint8_t> initDevicePreset(const device::Descriptor& desc);

enum class DevicePresetLoad {
	Loaded,
	/// Not a rack patch holding exactly one device.
	Malformed,
	/// A preset of a DIFFERENT device — refused rather than applied field-by-index into the wrong controls.
	OtherDevice,
	/// The container would not take the body (a chain refused it, or no running instance to give it to).
	/// Nothing was changed: the body is applied first, and fields only once it has been taken.
	BodyRefused,
	/// THE DEPTH GATE (`rack::fitsNesting`): its chains would stand deeper than any file gives back. Nothing changed.
	TooDeep,
};

/// **PUT PRESET BYTES INTO THE DEVICE.** All or nothing: the bytes are parsed and checked against
/// `desc`'s tag, then the body is applied, then the fields — so a refusal at any step leaves the device
/// as it was. A control the preset does not carry (a build that declared fewer) takes its DECLARED
/// DEFAULT, not whatever the device held: a preset is the whole sound, so an absent value is the
/// device's default rather than the previous sound leaking through.
///
/// **BY KEY SINCE BACKLOG JF**: a control the device gained since the preset was saved takes its default,
/// one it lost is ignored, a Choice finds its label wherever it now stands — and `report` (if given) says
/// every such departure, as a load report (`rack::LoadReport`).
DevicePresetLoad applyDevicePreset(backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                                   std::span<const uint8_t> bytes, std::span<const uint8_t> keep = {},
                                   rack::LoadReport* report = nullptr);

/// What a preset holds, for a reader that wants the values rather than the effect — the tests, which
/// assert what was SAVED rather than inferring it from what a load did. False as `applyDevicePreset`'s
/// `Malformed`. `device` is the keyed record as the file holds it; `field` is its row for `desc` (by key,
/// through `rack::rowOfValues`), and empty when no `desc` is given or it is another device's.
struct DevicePresetContents {
	std::string tag;
	rack::RackPatch::Device device;
	std::vector<float> field;
	std::vector<uint8_t> body;
	/// Its site, when the preset carries one.
	std::optional<DevicePresetSite> site;
};
bool readDevicePreset(std::span<const uint8_t> bytes, DevicePresetContents* out,
                      const device::Descriptor* desc = nullptr, rack::LoadReport* report = nullptr);

/// **THE FOLDER A DEVICE'S BANK LIVES IN, UNDER THE PRESET ROOT** — `Devices/<tag>`, beside the plugin's own `Feathers/` (`preset_store.h`). The tag
/// rather than the display name because the tag is what a preset is checked against on load, so the
/// folder and the check cannot disagree; sanitised the way a preset name is.
std::string devicePresetBank(const device::Descriptor& desc);

} // namespace feathers
