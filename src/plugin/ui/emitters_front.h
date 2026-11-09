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

#include "device/registry.h"

#include <functional>
#include <utility>
#include <vector>

// ARPS SENT TO FRONT OF CHAIN: a setting, true by default.
//
// While ON, a device that EMITS notes on its own clock (`NoteOps::emitNotes` — every arp, today's and later ones; not
// a name check) picked from an add menu lands at the FRONT of the lane or chain it was added to, ahead of every
// modulator, because an arp's steps are what the devices after it should hear. OFF, it appends like anything else.
// An "Add before…" names its place and is honoured either way; dragging stays free.
//
// **AN EDITOR PREFERENCE, NOT PATCH STATE** — the tooltip detail's and the window-controls side's road
// (`emitters_front_prefs.h`): it decides how a HAND places a device, which is the player's habit and not the sound of
// any patch, and a patch loaded on another desk must not change how that desk's picker behaves. One value for the
// process, a watch per editor writing it to its prefs, both editors (Feathers and the rack) restoring it.

namespace feathers::ui {

/// **DOES THIS DEVICE EMIT NOTES ON ITS OWN CLOCK** — the capability, so a future arp joins by declaring it. A CONTAINER
/// that carries its chains' notes (a Rack, backlog RN) is not one: it is placed where a hand puts it, as before.
[[nodiscard]] constexpr bool emitsNotes(const device::Descriptor& desc) noexcept {
	return desc.notes != nullptr && desc.notes->emitNotes != nullptr && desc.container == nullptr;
}

class EmittersFrontWatch;

namespace emitters_front_detail {
inline bool& slot() noexcept {
	static bool on = true; // true by default
	return on;
}
inline std::vector<EmittersFrontWatch*>& watches() {
	static std::vector<EmittersFrontWatch*> list;
	return list;
}
} // namespace emitters_front_detail

/// Whether a picked arp goes to the front. Main thread.
[[nodiscard]] inline bool emittersToFront() noexcept {
	return emitters_front_detail::slot();
}

/// One editor's ear on the setting, for its prefs.
class EmittersFrontWatch {
public:
	explicit EmittersFrontWatch(std::function<void(bool)> changed) : changed_(std::move(changed)) {
		emitters_front_detail::watches().push_back(this);
	}
	~EmittersFrontWatch() { std::erase(emitters_front_detail::watches(), this); }
	EmittersFrontWatch(const EmittersFrontWatch&) = delete;
	EmittersFrontWatch& operator=(const EmittersFrontWatch&) = delete;

private:
	friend size_t setEmittersToFront(bool on);
	std::function<void(bool)> changed_;
};

/// Set it for the process; returns how many watches were told (0 when it did not move).
inline size_t setEmittersToFront(bool on) {
	if (on == emitters_front_detail::slot()) {
		return 0;
	}
	emitters_front_detail::slot() = on;
	const std::vector<EmittersFrontWatch*> told = emitters_front_detail::watches();
	for (EmittersFrontWatch* w : told) {
		if (w->changed_) {
			w->changed_(on);
		}
	}
	return told.size();
}

/// **WHERE A PICKED DEVICE GOES: `true` is "in front of the first device"**. Only for a pick with no place of its own
/// (an "Add before…" has one), an emitter, the setting on, and something already there to go in front of.
[[nodiscard]] inline bool pickGoesToFront(const device::Descriptor* desc, bool hasAnchor, size_t devicesThere) noexcept {
	return !hasAnchor && desc != nullptr && emitsNotes(*desc) && emittersToFront() && devicesThere > 0;
}

} // namespace feathers::ui
