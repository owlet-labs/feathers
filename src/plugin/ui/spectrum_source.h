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

#include "device/modulatable.h"
#include "dsp/spectral_shaper.h"
#include "plugin/params.h"
#include "plugin/spectral_plot.h"
#include "plugin/ui/frame_watchers.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

// **WHAT THE SPECTRUM EDITOR READS AND WRITES** — one interface, two adapters (a second, re-written drawer had led to a
// divergent UX; decided as Option A). The plot (`SpectrumPlot`) and the operator
// row (`SpectrumEditor`) are written against this, never against the bridge or a device host, so the Additive drawer and the
// Spectral effect device draw the SAME panels, the SAME curve controls and the SAME handles:
//
//   - `BridgeSpectrumSource`: the Additive drawer, eight sounds' params through the ParamBridge (what the panel always read);
//   - `fx::spectral`'s device source (spectral_source.h): one set, the device's own controls through its DeviceHost.
//
// Values and writes are by `SpectralControl` (params.h), the drawer's vocabulary; a source answers only for the controls it
// has (`has`), and the editor builds no panel, knob or handle for the rest.

namespace feathers {
class ParamBridge;
}

namespace feathers::ui {

class Knob;
class ModState;
class EditHistory;
class StoredSwitch;
class Notifier;
class SegmentedControl;
class WidgetRegistry;

class SpectrumSource {
public:
	virtual ~SpectrumSource() = default;

	/// How many sets of controls the editor builds (the drawer's sounds; 1 for a device).
	[[nodiscard]] virtual size_t slots() const = 0;
	/// Whether this source has control `c` at all.
	[[nodiscard]] virtual bool has(SpectralControl c) const = 0;
	/// The master switch: the drawer's, or the device's Enabled.
	[[nodiscard]] virtual bool master() const = 0;
	/// Control `c` of set `slot`, in the drawer's units (log2 Hz for a cutoff, a Choice's index, a switch's 0/1).
	[[nodiscard]] virtual double value(size_t slot, SpectralControl c) const = 0;
	/// **WHETHER SET `slot`'S FILTER SITS AT ITS OPEN END** (Z7d: `spectral::filterOpenness` is 1), where it is off and its Resonance,
	/// Slope and Key Track change nothing — the honesty rule greys them. By the CONTROL here (Low Pass at its top, High Pass at its
	/// bottom): the corner Key Track moves depends on the note, which a face cannot know. A source whose f0 is fixed says more.
	[[nodiscard]] virtual bool filterOpen(size_t slot, int filter = 0) const;
	/// **THE FILTER TAB** (F2, additive design §3.6): which filter's controls the editor shows, 0 or 1. VIEW STATE, never saved and
	/// no control (it changes no sound: outside the window ⊇ card guard, as the EQ's band selection is). Kept by the source, so the
	/// drawer's panel and its plot agree; a device's source shares it between its card and window.
	[[nodiscard]] virtual int selectedFilter() const { return selectedFilter_; }
	virtual void selectFilter(int filter) { selectedFilter_ = filter == 1 ? 1 : 0; }
	/// Control `c`'s tooltip, as its own declaration says it (a handle on the plot answers with it); null when it has none.
	[[nodiscard]] virtual const char* tooltip(size_t slot, SpectralControl c) const = 0;
	/// Control `c`'s declared range, for a clamp or a bisection over it; false for a control this source lacks.
	virtual bool range(size_t slot, SpectralControl c, double& lo, double& hi) const = 0;

	/// A WRITABLE CONTROL for a handle's gesture or a widget's binding, owned by the caller.
	[[nodiscard]] virtual std::unique_ptr<device::Modulatable> control(size_t slot, SpectralControl c) = 0;
	/// THE ROW'S KNOB over `c` (subscribed for redraws where the source has subscriptions).
	[[nodiscard]] virtual std::unique_ptr<Knob> knob(size_t slot, SpectralControl c, const char* caption, Notifier* notifier) = 0;
	/// THE ROW'S SEGMENTED CONTROL over the Choice `c` (the filter's Mode).
	[[nodiscard]] virtual std::unique_ptr<SegmentedControl> segmented(size_t slot, SpectralControl c,
	                                                                   std::span<const char* const> names, Notifier* notifier) = 0;
	/// Subscribe `frame` to redraw when `c` moves, where the source has subscriptions (the bridge's); the token goes to `into`.
	virtual void watch(size_t slot, SpectralControl c, visage::Frame* frame, std::vector<Watch>& into) {
		(void)slot;
		(void)c;
		(void)frame;
		(void)into;
	}

	/// THE DRAWN CURVE, as gains per harmonic band (the shaper's), and its switch.
	[[nodiscard]] virtual spectral::DrawnCurve curve(size_t slot) const = 0;
	virtual void setCurveOn(size_t slot, bool on) = 0;
	virtual void setCurveBand(size_t slot, size_t band, float gain) = 0;
	virtual void flattenCurve(size_t slot) = 0;
	/// A writer of the whole curve that OUTLIVES the editor, for an undo record (EditHistory keeps records past a panel).
	[[nodiscard]] virtual std::function<void(const spectral::DrawnCurve&)> curveWriter(size_t slot) = 0;
	/// THE CURVE'S SWITCH as a control a chip draws: the drawer's stored state (a StoredSwitch, one undo step a flip), or the
	/// device's Curve control. Owned by the caller.
	[[nodiscard]] virtual std::unique_ptr<device::Modulatable> curveSwitch(size_t slot) = 0;
	/// THE EDITOR'S MODULATION STATE AND UNDO HISTORY, for the controls this source made (the curve switches' history; a
	/// device's controls' routes).
	virtual void bindModulation(ModState* mod, EditHistory* history) {
		(void)mod;
		(void)history;
	}

	/// **A LINE THE PLOT DRAWS OVER THE BARS** (the Spectral vocoder's sidechain envelope, design §10.5): up to `max` points of
	/// (Hz, dB) in `hz`/`db`, the line's shape referred to its own peak. Answers how many; 0 (the default, and the drawer's) draws none.
	virtual int overlay(float* hz, float* db, int max) const {
		(void)hz;
		(void)db;
		(void)max;
		return 0;
	}

	/// THE PLOT'S FRAME: a serial that moves when a new one is ready, and the frame itself.
	[[nodiscard]] virtual uint32_t frameSerial() const = 0;
	virtual bool readFrame(SpectralPlotFrame& out, uint32_t& serial) const = 0;

private:
	int selectedFilter_ = 0;
};

/// **THE ADDITIVE DRAWER'S SOURCE**: everything the spectrum panel read from the bridge before the interface existed, verbatim.
class BridgeSpectrumSource final : public SpectrumSource {
public:
	BridgeSpectrumSource(ParamBridge& bridge, WidgetRegistry* registry) : bridge_(bridge), registry_(registry) {}

	size_t slots() const override { return kMaxSounds; }
	bool has(SpectralControl) const override { return true; }
	bool master() const override;
	double value(size_t slot, SpectralControl c) const override;
	bool range(size_t slot, SpectralControl c, double& lo, double& hi) const override;
	const char* tooltip(size_t slot, SpectralControl c) const override;
	std::unique_ptr<device::Modulatable> control(size_t slot, SpectralControl c) override;
	std::unique_ptr<Knob> knob(size_t slot, SpectralControl c, const char* caption, Notifier* notifier) override;
	std::unique_ptr<SegmentedControl> segmented(size_t slot, SpectralControl c, std::span<const char* const> names,
	                                            Notifier* notifier) override;
	void watch(size_t slot, SpectralControl c, visage::Frame* frame, std::vector<Watch>& into) override;
	spectral::DrawnCurve curve(size_t slot) const override;
	void setCurveOn(size_t slot, bool on) override;
	void setCurveBand(size_t slot, size_t band, float gain) override;
	void flattenCurve(size_t slot) override;
	std::function<void(const spectral::DrawnCurve&)> curveWriter(size_t slot) override;
	std::unique_ptr<device::Modulatable> curveSwitch(size_t slot) override;
	void bindModulation(ModState* mod, EditHistory* history) override;
	uint32_t frameSerial() const override;
	bool readFrame(SpectralPlotFrame& out, uint32_t& serial) const override;

	[[nodiscard]] ParamBridge& bridge() const { return bridge_; }

private:
	ParamBridge& bridge_;
	WidgetRegistry* registry_ = nullptr;
	/// The curve switches this source made, for their undo history (bindModulation). They live in the editor's sets.
	std::vector<StoredSwitch*> curveSwitches_;
};

} // namespace feathers::ui
