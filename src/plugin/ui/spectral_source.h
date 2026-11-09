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

#include "backplane/device_host.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/spectrum_source.h"

#include <array>
#include <array>
#include <memory>
#include <optional>
#include <vector>

// **THE SPECTRAL DEVICE AS A SPECTRUM SOURCE** (spectrum_source.h): the Additive drawer's plot and operator panels over the
// device's own controls, so the card and the window draw what the drawer draws (option A).
//
// THE MAPPING: the drawer's Filter (no Key track knob: the device's cutoff always follows f0), Comb, Tilt, Odd/Even and Vowel are
// the device's; Formant Shift, Partials, Damp, Stretch and Shift are not (`has` answers false, and the editor builds nothing for
// them). The curve is the device's, stored in dB and handed out as the drawer's gains.
//
// THE FRAME the plot draws is made here from the device's readings: the partials of f0 (the reading the device publishes —
// Key Track's note or C3, raised by Offset), each at the level the device's input spectrum has at its frequency, and the result
// through the device's own operators (the shaper's gains, compensated over that terrain as the drawer's is). With no input yet,
// a sawtooth's 1/k stands in, so the operators still have something to draw against; the label then says "reference".

namespace feathers::fx::spectral {

class DeviceSpectrumSource final : public ui::SpectrumSource {
public:
	DeviceSpectrumSource(backplane::DeviceHost& host, ui::WidgetRegistry& registry, DeviceInstanceId id);
	~DeviceSpectrumSource() override;
	DeviceSpectrumSource(const DeviceSpectrumSource&) = delete;
	DeviceSpectrumSource& operator=(const DeviceSpectrumSource&) = delete;

	size_t slots() const override { return 1; }
	bool has(SpectralControl c) const override { return paramOf(c).has_value(); }
	bool master() const override;
	double value(size_t slot, SpectralControl c) const override;
	bool range(size_t slot, SpectralControl c, double& lo, double& hi) const override;
	/// WITH KEY TRACK OFF f0 IS FIXED (C3 raised by Offset), so the corner's road counts too: Offset +12 with the control an octave
	/// short of its top puts the corner at the plot's edge, and the filter is open (Z7d).
	bool filterOpen(size_t slot, int filter = 0) const override;
	/// THE FILTER TAB, shared by this device's card and window (never saved: view state).
	int selectedFilter() const override;
	void selectFilter(int filter) override;
	const char* tooltip(size_t slot, SpectralControl c) const override;
	std::unique_ptr<device::Modulatable> control(size_t slot, SpectralControl c) override;
	std::unique_ptr<ui::Knob> knob(size_t slot, SpectralControl c, const char* caption, ui::Notifier* notifier) override;
	std::unique_ptr<ui::SegmentedControl> segmented(size_t slot, SpectralControl c, std::span<const char* const> names,
	                                                ui::Notifier* notifier) override;
	::feathers::spectral::DrawnCurve curve(size_t slot) const override;
	void setCurveOn(size_t slot, bool on) override;
	void setCurveBand(size_t slot, size_t band, float gain) override;
	void flattenCurve(size_t slot) override;
	std::function<void(const ::feathers::spectral::DrawnCurve&)> curveWriter(size_t slot) override;
	std::unique_ptr<device::Modulatable> curveSwitch(size_t slot) override;
	void bindModulation(ui::ModState* mod, ui::EditHistory* history) override;
	uint32_t frameSerial() const override;
	bool readFrame(SpectralPlotFrame& out, uint32_t& serial) const override;
	int overlay(float* hz, float* db, int max) const override;
	/// Whether the device is vocoding as its readings say: Vocode on and a sidechain connected.
	[[nodiscard]] bool vocoding() const;
	/// Whether its readings say a sidechain is connected.
	[[nodiscard]] bool sidechainSounding() const;

	/// THE DEVICE'S OWN CONTROL `p`, one per control and owned here, for the widgets the card and window build beside the panels.
	[[nodiscard]] ui::DeviceControl& deviceControl(Param p);
	/// The drawer's control `c` as the device's, if it has one.
	[[nodiscard]] static std::optional<Param> paramOf(SpectralControl c);
	/// The device's settings as stored (the row), and the frame's f0 as the device published it (C3 before any reading).
	[[nodiscard]] Settings settings() const;
	/// THE DRAWN CURVE IN dB, from the device's hand block (`DeviceHost::blockAt`); flat before anything is drawn.
	[[nodiscard]] std::array<float, ::feathers::spectral::kCurveBands> curveDb() const;
	/// The block's index in `kBlocks` (`spectral_curve_db`).
	static constexpr size_t kCurveBlock = 1;
	[[nodiscard]] double f0() const;

private:
	[[nodiscard]] float field(size_t index) const;
	/// Build the frame from the readings and the settings now (readFrame's work); `playing` as the readings say.
	void makeFrame(SpectralPlotFrame& out) const;

	backplane::DeviceHost& host_;
	ui::WidgetRegistry& registry_;
	DeviceInstanceId device_ = kNoInstance;
	ui::ModState* mod_ = nullptr;
	ui::EditHistory* history_ = nullptr;
	std::array<std::unique_ptr<ui::DeviceControl>, kParamCount> controls_ {};
	/// What the last frame was made from (the readings and the row), and its serial: a change moves the serial.
	mutable std::vector<float> lastInputs_;
	mutable uint32_t serial_ = 0;
	mutable bool primed_ = false;
};

} // namespace feathers::fx::spectral
