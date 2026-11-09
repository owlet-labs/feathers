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

#include "plugin/ui/mod_state.h"

namespace feathers::ui {

/// **AN INNER ARM, MIRRORED INTO THE HOST'S STATE AS A BORROWED ARM** (backlog NZ rung 4, then NZ (b)). A panel's own
/// `ModState` is local to it, and the host's lane cards answer to the host's. So while a hand holds a source armed inside
/// a panel whose sources reach out the back — a site's PRE (spelled as the holder's export), a Chain's or a Rack's inner
/// modulator (spelled as the container's export) — the host's state is armed for that spelling, and every host card that
/// may take the route volunteers through `mayRoute` as for any source.
///
/// BORROWED, so it clears nobody's arm, and PUT BACK AS FOUND: disarmed only while the host still holds what this put
/// there, and refocused on the source it had before, so a hand that armed something else in the host meanwhile keeps it.
/// One per panel; it lets go when the panel goes.
class BorrowedArmRelay {
public:
	BorrowedArmRelay() = default;
	BorrowedArmRelay(const BorrowedArmRelay&) = delete;
	BorrowedArmRelay& operator=(const BorrowedArmRelay&) = delete;
	~BorrowedArmRelay() { release(); }

	/// THE HOST'S STATE, as the panel was bound to it. A relay held on the old one is put back first.
	void setTarget(ModState* outer) {
		if (outer != outer_) {
			release();
			outer_ = outer;
		}
	}

	/// Arm the host for `want`, or put it back when `want` is nobody. True when the host's state moved.
	bool relay(device::Source want) {
		if (outer_ == nullptr) {
			return false;
		}
		if (want == device::Source::nobody()) {
			return release();
		}
		if (relayed_ && relayedSource_ == want && outer_->armedLocally() && outer_->source() == want) {
			return false;
		}
		if (!relayed_) {
			outerWas_ = outer_->source();
		}
		relayed_ = true;
		relayedSource_ = want;
		outer_->focus(want);
		(void)outer_->arm(true, ModState::Arm::Borrowed);
		return true;
	}

	/// Put the host back as it was found. True when it moved.
	bool release() {
		if (!relayed_ || outer_ == nullptr) {
			relayed_ = false;
			return false;
		}
		relayed_ = false;
		if (outer_->source() == relayedSource_) {
			(void)outer_->arm(false, ModState::Arm::Borrowed);
			outer_->focus(outerWas_);
		}
		return true;
	}

	[[nodiscard]] bool relayed() const noexcept { return relayed_; }

private:
	ModState* outer_ = nullptr;
	bool relayed_ = false;
	device::Source relayedSource_ = device::Source::nobody();
	device::Source outerWas_ = device::Source::nobody();
};

} // namespace feathers::ui
