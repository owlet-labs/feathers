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

// THE EQ'S STRIP AND GLOBALS — the widgets the card and the panel
// share, laid out by each into its own rectangles.
//
// ONE WIDGET SET PER BAND, only the selected band's shown. A `Knob` or `ValueBox` holds its control by
// reference for life and cannot be rebound, so "the strip rebinds to the selected band" is eight strips of
// which one is visible. That also keeps every band's knobs as real modulation destinations.
//
// THE HONESTY RULE (A8, resolved here): a control is shown enabled only while it changes the sound.
//   - An OFF band shows only its On switch; the owner writes "Band n is off" beside it.
//   - Gain is shown only for a type that has one; Slope only for a cut (the descriptor's gates).
//   - Channel (Stereo / Mid / Side, step 6) is shown with the rest of an ON band's strip.
//   - The tilt pivot is shown only while there is a tilt (on the plot, its diamond likewise).
//   - A 6 dB cut's Q, and Offset / Amount while nothing they act on is
//     active, are GREYED but left live (`Knob::setActive(false)`), so they can be set up in advance
//     (assumption A11).

#include "backplane/device_host.h"
#include "fx/eq/eq_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace feathers::ui {

/// ONE BAND'S CHANNEL — Stereo, Mid or Side (step 6) — as a control a `ValueBox` can hold. It lives in the
/// extra-state block, not the params (R1), so it is no host parameter and no modulation destination. **IT
/// CHANGES THE SOUND, SO A PICK IS ONE UNDO STEP** (backlog JB, overturning assumption A13 for this field:
/// the rest of that block — the band selection and the analyser's display — is view state and records
/// nothing). Recorded into the history `setHistory` hands it, by device id and field.
class EqChannelControl final : public device::Modulatable, public RecordsEdits {
public:
	EqChannelControl(backplane::DeviceHost& host, DeviceInstanceId id, int band);

	const device::ParamDesc& desc() const override;
	double value() const override;
	void setValue(double value) override;
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override;
	void endGesture() override;
	std::string text() const override;

	void setHistory(EditHistory* history) noexcept { history_ = history; }
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	size_t field_;
	EditHistory* history_ = nullptr;
	double valueBefore_ = 0.0;
};

class EqControls {
public:
	using ControlFn = std::function<DeviceControl&(size_t)>;

	EqControls(visage::Frame& parent, backplane::DeviceHost& host, DeviceInstanceId id, ControlFn control,
	           Notifier* notifier, float knobDiameter, bool withPivotBox);

	/// Lay the selected band's strip into `strip` (a 16 px row over a knob row) and the globals into
	/// `globals`; hide everything else. `open` false hides it all (a folded card).
	void layOut(visage::Bounds strip, visage::Bounds globals, int selected, bool open);

	/// THE SHAPE the honesty rule reads — selected band, its on/type/slope, tilt present, what Offset and
	/// Amount act on. When it changes the owner re-lays out.
	[[nodiscard]] std::vector<int> honestyKey(int selected) const;

	/// Where the owner writes "Band n is off", or an empty rectangle.
	[[nodiscard]] visage::Bounds offCaption() const { return offCaption_; }

	void setModulation(ModState* mod, EditHistory* history);

	[[nodiscard]] float rowHeight() const { return Knob::columnHeight(diameter_); }
	static constexpr float kSwitchRow = 16.0f;
	static constexpr float kGap = 4.0f;
	/// The Channel box's width: "Stereo" in the compact box's face.
	static constexpr float kChannelWidth = 54.0f;

	/// Every control some widget here reaches (the enable is the head's).
	[[nodiscard]] std::vector<size_t> addressedParams() const;

	// TEST SEAMS
	[[nodiscard]] Knob* knobForTest(size_t param) const;
	[[nodiscard]] ValueBox* boxForTest(size_t param) const;
	[[nodiscard]] SwitchChip* chipForTest(size_t param) const;
	[[nodiscard]] ValueBox* channelBoxForTest(int band) const { return bands_[static_cast<size_t>(band)].channel.get(); }

private:
	struct BandWidgets {
		std::unique_ptr<SwitchChip> on;
		std::unique_ptr<ValueBox> type;
		std::unique_ptr<ValueBox> slope;
		std::unique_ptr<Knob> freq;
		std::unique_ptr<Knob> gain;
		std::unique_ptr<Knob> q;
		std::unique_ptr<EqChannelControl> channelControl;
		std::unique_ptr<ValueBox> channel;
	};
	[[nodiscard]] float field(size_t i) const;
	[[nodiscard]] bool anyBandActing() const;
	[[nodiscard]] bool anyGainActing() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	ControlFn control_;
	float diameter_;
	std::array<BandWidgets, fx::eq::kBandCount> bands_ {};
	std::unique_ptr<Knob> offset_, amount_, tilt_, output_;
	std::unique_ptr<ValueBox> pivot_;
	std::unique_ptr<SwitchChip> phase_;
	std::unique_ptr<SwitchChip> keyTrack_;
	visage::Bounds offCaption_ {};
};

} // namespace feathers::ui
