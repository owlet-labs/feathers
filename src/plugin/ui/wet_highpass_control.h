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

#include "plugin/ui/device_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <memory>

// **HP WET AND SUB THRU, ONE ELEMENT FOR EVERY DEVICE THAT OFFERS THEM** (the Eroder, the Smooth Shaper): the HP Wet
// knob (hertz, and where a device tracks the played note, a second knob in semitones that takes its place), and the
// Sub Thru switch. The engine half is `fx/wet_highpass.h`. A device builds one over its own controls, lays the pieces
// out where its window wants them (the Eroder seats the chip in its block diagram), and calls `refresh` with what its
// engine says; the element decides the one thing that is shared, WHEN SUB THRU IS ENABLED: only while the filter is in
// the signal (and the device has dry to give back).

namespace feathers::ui {

/// **WHETHER THE WET HIGHPASS IS IN THE SIGNAL** for these control values: with `semisLive` (the device tracks the note and
/// Track is on) the semitones decide, else the hertz. Each control's bottom is "no filter".
[[nodiscard]] inline bool wetHighpassFiltering(double hertzLog2, double hertzLog2Min, bool semisLive, double semis,
                                               double semisMin) noexcept {
	return semisLive ? semis > semisMin + static_cast<double>(1.0e-3f) : hertzLog2 > hertzLog2Min;
}

class WetHighpassControl {
public:
	/// @param semitones  the semitone control of a device that tracks the played note, else null (hertz only)
	WetHighpassControl(device::Modulatable& hertz, device::Modulatable* semitones, device::Modulatable& subThru,
	                   Notifier* notifier, float faceDiameter) {
		hertz_ = std::make_unique<Knob>(hertz, "HP Wet", notifier);
		hertz_->setFaceDiameter(faceDiameter);
		if (semitones != nullptr) {
			semis_ = std::make_unique<Knob>(*semitones, "HP Wet", notifier);
			semis_->setFaceDiameter(faceDiameter);
		}
		subThru_ = std::make_unique<SwitchChip>(subThru, "Sub Thru");
	}

	[[nodiscard]] Knob* hertz() const { return hertz_.get(); }
	/// Null when the device has no tracking.
	[[nodiscard]] Knob* semitones() const { return semis_.get(); }
	[[nodiscard]] SwitchChip* subThru() const { return subThru_.get(); }

	/// The knobs become children of `parent`; the chip too unless the device seats it elsewhere.
	void addTo(visage::Frame& parent, bool seatChip = false) {
		parent.addChild(hertz_.get());
		if (semis_) {
			parent.addChild(semis_.get());
		}
		if (!seatChip) {
			parent.addChild(subThru_.get());
		}
	}

	void setModulation(ModState* mod, EditHistory* history) {
		hertz_->setModulation(mod, history);
		if (semis_) {
			semis_->setModulation(mod, history);
		}
	}

	/// THE TWO FACES OF ONE CELL: hertz, or semitones while `semisLive`. Hidden, not greyed. `shown` is the cell's own.
	void showFaces(bool shown, bool semisLive) {
		hertz_->setVisible(shown && !(semisLive && semis_));
		if (semis_) {
			semis_->setVisible(shown && semisLive);
		}
	}

	/// SUB THRU HONESTY: it gives back what the filter took of a dry that was turned down, so with no filter, or with no dry to
	/// take from (`dryTurnedDown` false: the device's dry is whole), it changes nothing and is greyed.
	void refresh(bool filtering, bool dryTurnedDown) { subThru_->setEnabled(filtering && dryTurnedDown); }

private:
	std::unique_ptr<Knob> hertz_;
	std::unique_ptr<Knob> semis_;
	std::unique_ptr<SwitchChip> subThru_;
};

} // namespace feathers::ui
