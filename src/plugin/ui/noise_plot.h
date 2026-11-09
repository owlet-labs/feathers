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
#include "fx/noise/noise_dsp.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <memory>
#include <vector>

// NOISE'S PICTURE — what is added, drawn around what you hear (backlog NA: the filter curve over the noise
// colour's spectrum, with the impulses shown as a scatter of hits).
//
// **WHAT IT DRAWS IS THE DEVICE'S OWN DESIGNS.** The spectrum is `Tilt::gain` x `Svf::gain` (`noise_dsp.h`) at
// the device's rate — the coefficients the DSP installs, normalisation included — so white reads 0 dB and a
// tilt pivots about the loudness it holds. Under Impulses the source is flat and the filter is the spectrum,
// and the foot of the plot is a strip of hits: two seconds of the events the device's own generator
// (`dsp::RandomEvents`) makes from the current settings and seed, so moving Density, Timing or Hit Size moves
// the scatter the way it moves the sound. Under Free the strip is a sample of what the generator does, since a
// free stream has no fixed pattern to draw.
//
// **THE FILTER'S HANDLE IS ON THE PLOT**: a press places the corner under the pointer and a drag moves it — x is
// Cutoff, height is Resonance. With a source armed the press is a ROUTE drag on Cutoff, `Knob`'s gesture.

namespace feathers::ui {

class ModState;
class EditHistory;

class NoisePlot final : public visage::Frame, public TooltipSource {
public:
	NoisePlot(backplane::DeviceHost& host, DeviceInstanceId id);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	void setModulation(ModState* mod, EditHistory* history);

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did — the card's `updateContent` contract.
	bool update();

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — the plotted VALUES, so a case asserts the picture's numbers.                      //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] size_t pointCountForTest() const noexcept { return spectrumDb_.size(); }
	[[nodiscard]] double hzAtForTest(size_t i) const noexcept { return hzAt(i); }
	/// The drawn spectrum at point i, in dB (white at 0).
	[[nodiscard]] double spectrumDbForTest(size_t i) const noexcept { return spectrumDb_[i]; }
	/// The hits in the strip: their times (s) and values.
	[[nodiscard]] const std::vector<float>& hitTimesForTest() const noexcept { return hitTimes_; }
	[[nodiscard]] const std::vector<float>& hitValuesForTest() const noexcept { return hitValues_; }
	[[nodiscard]] float hzToXForTest(double hz) const noexcept { return hzToX(hz); }
	[[nodiscard]] double xToHzForTest(float x) const noexcept { return xToHz(x); }
	[[nodiscard]] float resonanceToYForTest(float res) const noexcept { return resonanceToY(res); }
	[[nodiscard]] bool showsHitsForTest() const noexcept { return drawn_.source == static_cast<int>(fx::noise::Source::Impulses); }

	static constexpr double kMinHz = 20.0;
	static constexpr double kMaxHz = 20000.0;
	/// THE SPECTRUM AXIS: white at 0 dB, from `kFloorDb` to `kTopDb`.
	static constexpr double kTopDb = 24.0;
	static constexpr double kFloorDb = -36.0;
	/// HOW LONG A STRETCH OF HITS THE STRIP SHOWS.
	static constexpr double kStripSeconds = 2.0;
	/// AT MOST THIS MANY HITS ARE DRAWN — a dense stream is a texture, and past this it reads as one.
	static constexpr size_t kMaxHits = 400;
	static constexpr float kStripHeight = 20.0f;

private:
	[[nodiscard]] DeviceControl& control(fx::noise::Param p) const;
	[[nodiscard]] fx::noise::Settings settingsNow() const;
	[[nodiscard]] double rate() const noexcept;
	[[nodiscard]] double hzAt(size_t i) const noexcept;
	[[nodiscard]] float hzToX(double hz) const noexcept;
	[[nodiscard]] double xToHz(float x) const noexcept;
	[[nodiscard]] float dbToY(double db) const noexcept;
	[[nodiscard]] float resonanceToY(float res) const noexcept;
	[[nodiscard]] float yToResonance(float y) const noexcept;
	[[nodiscard]] float plotTop() const noexcept;
	[[nodiscard]] float plotFoot() const noexcept;
	[[nodiscard]] bool impulses() const noexcept;
	void recompute(const fx::noise::Settings& s);
	void place(const visage::MouseEvent& e);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	std::vector<double> spectrumDb_;
	std::vector<double> filterDb_;
	std::vector<float> hitTimes_;
	std::vector<float> hitValues_;

	bool computed_ = false;
	fx::noise::Settings drawn_ {};
	double drawnRate_ = 0.0;
	bool drawnArmed_ = false;
	float drawnAmount_ = 0.0f;

	struct Drag {
		bool active = false;
		bool route = false;
		float startY = 0.0f;
		float anchor = 0.0f;
	} drag_ {};
};

} // namespace feathers::ui
