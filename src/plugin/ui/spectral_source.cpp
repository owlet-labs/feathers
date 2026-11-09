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

#include "plugin/ui/spectral_source.h"

#include "dsp/partial_spectrum.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/spectrum_axes.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace feathers::fx::spectral {

namespace {

/// A curve gain as the device stores it (dB): exact silence at the floor, the drawer's top held to the device's.
float dbOfGain(float gain) {
	if (!(gain > 0.0f)) {
		return kCurveFloorDb;
	}
	return std::clamp(static_cast<float>(20.0 * std::log10(static_cast<double>(gain))), kCurveFloorDb, kCurveMaxDb);
}

} // namespace

namespace {
/// **THE FILTER TAB EACH DEVICE SHOWS** (F2, additive design §3.6): view state, never saved, shared by the device's card and its
/// window — each builds its own source (over whichever DeviceHost its surface holds; two hosts over one bridge are one device), so
/// it lives here, keyed by the instance id, and LIVES AS LONG AS A VIEW OF IT: the sources count themselves in and out, and the
/// last one out forgets it (a later device given the same id starts on Filter 1). Message thread only.
struct FilterTab {
	int tab = 0;
	int views = 0;
};
std::map<DeviceInstanceId, FilterTab>& filterTabs() {
	static std::map<DeviceInstanceId, FilterTab> tabs;
	return tabs;
}
} // namespace

DeviceSpectrumSource::DeviceSpectrumSource(backplane::DeviceHost& host, ui::WidgetRegistry& registry, DeviceInstanceId id)
    : host_(host), registry_(registry), device_(id) {
	++filterTabs()[device_].views;
}

DeviceSpectrumSource::~DeviceSpectrumSource() {
	auto& tabs = filterTabs();
	if (const auto it = tabs.find(device_); it != tabs.end() && --it->second.views <= 0) {
		tabs.erase(it);
	}
}

std::optional<Param> DeviceSpectrumSource::paramOf(SpectralControl c) {
	using C = SpectralControl;
	switch (c) {
	case C::FilterOn: return Param::FilterOn;
	case C::FilterMode: return Param::FilterMode;
	case C::FilterCutoff: return Param::FilterCutoff;
	case C::FilterResonance: return Param::FilterResonance;
	case C::FilterSlope: return Param::FilterSlope;
	case C::FilterGain: return Param::FilterGain;
	case C::Filter2On: return Param::Filter2On;
	case C::Filter2Mode: return Param::Filter2Mode;
	case C::Filter2Cutoff: return Param::Filter2Cutoff;
	case C::Filter2Resonance: return Param::Filter2Resonance;
	case C::Filter2Slope: return Param::Filter2Slope;
	case C::Filter2Gain: return Param::Filter2Gain;
	case C::FilterRouting: return Param::FilterRouting;
	case C::CombOn: return Param::CombOn;
	case C::CombSpacing: return Param::CombSpacing;
	case C::CombDepth: return Param::CombDepth;
	case C::CombPhase: return Param::CombPhase;
	case C::TiltOn: return Param::TiltOn;
	case C::TiltAmount: return Param::TiltAmount;
	case C::OddEvenOn: return Param::OddEvenOn;
	case C::OddEvenBalance: return Param::OddEvenBalance;
	case C::FormantOn: return Param::VowelOn;
	case C::FormantVowel: return Param::Vowel;
	case C::FormantDepth: return Param::VowelDepth;
	case C::FormantShiftOn: return Param::FShiftOn;
	case C::FormantShiftLow: return Param::FShiftLow;
	case C::FormantShiftHigh: return Param::FShiftHigh;
	case C::FormantShiftCrossover: return Param::FShiftCrossover;
	case C::FormantShiftLink: return Param::FShiftLink;
	default: return std::nullopt;
	}
}

float DeviceSpectrumSource::field(size_t index) const {
	if (index < host_.fieldsPresentAt(device_)) {
		return host_.fieldAt(device_, index);
	}
	return index < kParamCount ? static_cast<float>(kParamDescs[index].def) : 0.0f;
}

std::array<float, ::feathers::spectral::kCurveBands> DeviceSpectrumSource::curveDb() const {
	std::array<float, ::feathers::spectral::kCurveBands> db {}; // flat until a hand draws
	(void)host_.blockAt(device_, kCurveBlock, db);
	return db;
}

Settings DeviceSpectrumSource::settings() const {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = field(i);
	}
	Settings s = stateOf(row, row.size());
	s.curveDb = curveDb(); // THE CURVE IS THE HAND'S BLOCK, NOT THE ROW
	return s;
}

double DeviceSpectrumSource::f0() const {
	if (host_.readingCountAt(device_) >= kReadingCount) {
		const float f = host_.readingAt(device_, kReadingF0);
		if (f > 0.0f) {
			return f;
		}
	}
	return shaperF0(settings(), false, 0.0); // before a reading: C3 raised by Offset
}

bool DeviceSpectrumSource::master() const { return field(static_cast<size_t>(Param::Enabled)) >= 0.5f; }

bool DeviceSpectrumSource::filterOpen(size_t slot, int filter) const {
	const Settings s = settings();
	if (s.keyTrack) {
		return ui::SpectrumSource::filterOpen(slot, filter); // the note moves the corner: by the control alone
	}
	const double f0 = shaperF0(s, false, 0.0);
	const ::feathers::spectral::ShaperParams p = shaperParamsOf(s, f0);
	return ::feathers::spectral::filterOpenness(filter == 0 ? p.filter : p.filter2, f0) >= 1.0;
}

int DeviceSpectrumSource::selectedFilter() const {
	const auto& tabs = filterTabs();
	const auto it = tabs.find(device_);
	return it != tabs.end() ? it->second.tab : 0;
}

void DeviceSpectrumSource::selectFilter(int filter) {
	filterTabs()[device_].tab = filter == 1 ? 1 : 0;
}

double DeviceSpectrumSource::value(size_t, SpectralControl c) const {
	// THE CUTOFF ALWAYS FOLLOWS f0 (every operator does): the drawer's Key track, read as fully on.
	if (c == SpectralControl::FilterKeyTrack || c == SpectralControl::Filter2KeyTrack) {
		return 1.0;
	}
	const auto p = paramOf(c);
	return p.has_value() ? static_cast<double>(field(static_cast<size_t>(*p))) : 0.0;
}

bool DeviceSpectrumSource::range(size_t, SpectralControl c, double& lo, double& hi) const {
	const auto p = paramOf(c);
	if (!p.has_value()) {
		return false;
	}
	lo = kParamDescs[static_cast<size_t>(*p)].min;
	hi = kParamDescs[static_cast<size_t>(*p)].max;
	return true;
}

const char* DeviceSpectrumSource::tooltip(size_t, SpectralControl c) const {
	const auto p = paramOf(c);
	return p.has_value() ? kParamDescs[static_cast<size_t>(*p)].tooltip : nullptr;
}

ui::DeviceControl& DeviceSpectrumSource::deviceControl(Param p) {
	auto& slot = controls_[static_cast<size_t>(p)];
	if (slot == nullptr) {
		slot = std::make_unique<ui::DeviceControl>(host_, device_, kDescriptor, static_cast<size_t>(p), mod_, history_);
	}
	return *slot;
}

std::unique_ptr<device::Modulatable> DeviceSpectrumSource::control(size_t, SpectralControl c) {
	const auto p = paramOf(c);
	return p.has_value() ? std::make_unique<ui::DeviceControl>(host_, device_, kDescriptor, static_cast<size_t>(*p), mod_, history_)
	                     : nullptr;
}

std::unique_ptr<ui::Knob> DeviceSpectrumSource::knob(size_t, SpectralControl c, const char* caption, ui::Notifier* notifier) {
	return std::make_unique<ui::Knob>(deviceControl(*paramOf(c)), caption, notifier);
}

std::unique_ptr<ui::SegmentedControl> DeviceSpectrumSource::segmented(size_t, SpectralControl c,
                                                                      std::span<const char* const> names, ui::Notifier* notifier) {
	return std::make_unique<ui::SegmentedControl>(deviceControl(*paramOf(c)), registry_, names, notifier);
}

::feathers::spectral::DrawnCurve DeviceSpectrumSource::curve(size_t) const {
	::feathers::spectral::DrawnCurve c;
	c.on = field(static_cast<size_t>(Param::CurveOn)) >= 0.5f;
	const auto db = curveDb();
	for (size_t b = 0; b < c.gain.size(); ++b) {
		c.gain[b] = curveGainOf(std::clamp(db[b], kCurveFloorDb, kCurveMaxDb));
	}
	return c;
}

void DeviceSpectrumSource::setCurveOn(size_t, bool on) {
	host_.setField(device_, static_cast<size_t>(Param::CurveOn), on ? 1.0f : 0.0f);
}

void DeviceSpectrumSource::setCurveBand(size_t, size_t band, float gain) {
	if (band < static_cast<size_t>(::feathers::spectral::kCurveBands)) {
		auto db = curveDb();
		db[band] = dbOfGain(gain);
		(void)host_.setBlock(device_, kCurveBlock, db); // WHOLE, in one atomic write: the audio thread never sees half a drag step
	}
}

void DeviceSpectrumSource::flattenCurve(size_t) {
	const std::array<float, ::feathers::spectral::kCurveBands> flat {};
	(void)host_.setBlock(device_, kCurveBlock, flat);
}

std::function<void(const ::feathers::spectral::DrawnCurve&)> DeviceSpectrumSource::curveWriter(size_t) {
	backplane::DeviceHost* host = &host_; // the host outlives the window and every undo record, as DeviceControl's do
	const DeviceInstanceId id = device_;
	return [host, id](const ::feathers::spectral::DrawnCurve& c) {
		host->setField(id, static_cast<size_t>(Param::CurveOn), c.on ? 1.0f : 0.0f);
		std::array<float, ::feathers::spectral::kCurveBands> db {};
		for (size_t b = 0; b < c.gain.size(); ++b) {
			db[b] = dbOfGain(c.gain[b]);
		}
		(void)host->setBlock(id, kCurveBlock, db); // ONE WRITE: an undo or a redo restores the curve exactly, whole
	};
}

std::unique_ptr<device::Modulatable> DeviceSpectrumSource::curveSwitch(size_t) {
	return std::make_unique<ui::DeviceControl>(host_, device_, kDescriptor, static_cast<size_t>(Param::CurveOn), mod_, history_);
}

void DeviceSpectrumSource::bindModulation(ui::ModState* mod, ui::EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& c : controls_) {
		if (c != nullptr) {
			c->setModulation(mod, history);
		}
	}
}

void DeviceSpectrumSource::makeFrame(SpectralPlotFrame& out) const {
	out = SpectralPlotFrame {};
	const double base = f0();
	const bool readings = host_.readingCountAt(device_) >= kReadingCount;
	out.f0 = static_cast<float>(base);
	out.playing = readings && host_.readingAt(device_, kReadingPlaying) >= 0.5f ? 1 : 0;
	const int count = std::clamp(static_cast<int>(ui::spectrum_axes::kMaxHz / base), 0, SpectralPlotFrame::kPartials);
	out.count = count;
	// THE TERRAIN: the input spectrum's level at each partial, or a sawtooth's 1/k while the input is silent.
	bool any = false;
	if (readings) {
		for (size_t b = 0; b < static_cast<size_t>(kSpectrumBands); ++b) {
			any = any || host_.readingAt(device_, b) > kBandFloorDb;
		}
	}
	const double span = std::log2(kBandHighHz / kBandLowHz);
	std::array<float, SpectralPlotFrame::kPartials> k {}, hz {};
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		const double h = base * static_cast<double>(i + 1);
		out.ratio[u] = static_cast<float>(i + 1);
		k[u] = static_cast<float>(i + 1);
		hz[u] = static_cast<float>(h);
		if (any) {
			const double pos = std::clamp(std::log2(h / kBandLowHz) / span * kSpectrumBands, 0.0, kSpectrumBands - 1.0);
			const float db = host_.readingAt(device_, static_cast<size_t>(pos));
			out.terrain[u] = db > kBandFloorDb ? static_cast<float>(std::pow(10.0, db / 20.0)) : 0.0f;
		}
		else {
			out.terrain[u] = 1.0f / static_cast<float>(i + 1);
		}
	}
	// THE RESULT through the device's own operators at f0, compensated over this terrain as the drawer's is.
	const Settings s = settings();
	const ::feathers::spectral::PreparedShaper prepared = ::feathers::spectral::prepare(shaperParamsOf(s, base), base);
	::feathers::spectral::gains(prepared, k.data(), hz.data(), out.terrain.data(), count, out.left.data());
	// WHILE VOCODING, the vocoder's gain too (design §10.3): the carrier flattened by its envelope (the input bands' level at the
	// partial, or the reference terrain's), the sidechain's envelope imposed (its readings), as the device applies them.
	const bool voc = vocoding();
	const float flatten = std::clamp(s.vocodeFlatten, 0.0f, 1.0f);
	const auto bandDbAt = [&](size_t first, double hz) {
		const double pos = std::clamp(std::log2(hz / kBandLowHz) / span * kSpectrumBands - 0.5, 0.0, kSpectrumBands - 1.0);
		const auto b = std::min(static_cast<int>(pos), kSpectrumBands - 2);
		const double t = pos - b;
		return host_.readingAt(device_, first + static_cast<size_t>(b)) * (1.0 - t) + host_.readingAt(device_, first + static_cast<size_t>(b + 1)) * t;
	};
	// **PARTIALS CULLS AS THE DEVICE DOES (backlog OH: the engine's plot greys the culled partials; so does this)**: the kept ones
	// at their gain, the rest turned down by Depth — so at full Depth only the dark terrain bar is left where a partial is culled.
	// Harmonics keeps 1..⌈N⌉ (the last at N's fraction), Peaks the ⌈N⌉ loudest by the terrain (the input's level at the partial,
	// the stand-in for the device's strongest spectral peaks), as `Stft::partialsSpectrum` weighs them.
	std::array<float, SpectralPlotFrame::kPartials> cull {};
	cull.fill(1.0f);
	if (s.partialsOn && count > 0) {
		const double n = std::clamp(static_cast<double>(s.partialsCount), kPartialsMin, kPartialsMax);
		const int whole = std::min(count, static_cast<int>(std::ceil(n - 1e-9)));
		const float lastWeight = static_cast<float>(n - static_cast<double>(whole - 1));
		const float depth = std::clamp(s.partialsDepth, 0.0f, 1.0f);
		std::array<float, SpectralPlotFrame::kPartials> keep {};
		if (s.partialsMode == 2) {
			// CENTERS (OI): the fundamental and the set's few of 2..7, each by its weight from the generator the device keeps them by.
			for (int i = 0; i < std::min(count, 7); ++i) {
				keep[static_cast<size_t>(i)] = static_cast<float>(::feathers::partials::sparseKeep(i + 1, s.partialsSet));
			}
		}
		else if (s.partialsMode == 1) {
			for (int i = 0; i < whole; ++i) {
				keep[static_cast<size_t>(i)] = i == whole - 1 ? std::min(1.0f, lastWeight) : 1.0f;
			}
		}
		else {
			std::array<int, SpectralPlotFrame::kPartials> order {};
			for (int i = 0; i < count; ++i) {
				order[static_cast<size_t>(i)] = i;
			}
			const auto louder = [&](int a, int b) { return out.terrain[static_cast<size_t>(a)] > out.terrain[static_cast<size_t>(b)]; };
			std::nth_element(order.begin(), order.begin() + (whole - 1), order.begin() + count, louder);
			for (int r = 0; r < whole; ++r) {
				keep[static_cast<size_t>(order[static_cast<size_t>(r)])] = r == whole - 1 ? std::min(1.0f, lastWeight) : 1.0f;
			}
		}
		for (int i = 0; i < count; ++i) {
			const auto u = static_cast<size_t>(i);
			cull[u] = (1.0f - depth) + keep[u] * depth;
		}
	}
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		out.left[u] *= out.terrain[u] * cull[u];
		if (voc && out.terrain[u] > 0.0f) {
			const double ex = any ? std::pow(10.0, bandDbAt(0, hz[u]) / 20.0) : static_cast<double>(out.terrain[u]);
			const double es = std::pow(10.0, bandDbAt(kReadingSideEnvelope, hz[u]) / 20.0);
			out.left[u] *= static_cast<float>(ex > 0.0 ? std::pow(ex, -static_cast<double>(flatten)) * es : 0.0);
		}
		out.right[u] = out.left[u];
	}
}

uint32_t DeviceSpectrumSource::frameSerial() const {
	// THE FIRST ASK IS THE PLOT'S CONSTRUCTOR taking its "published before me" mark: answered 0, so the first tick's frame is new.
	if (!primed_) {
		primed_ = true;
		return 0;
	}
	// A NEW FRAME WHENEVER WHAT IT IS MADE FROM MOVES: the readings and the row.
	std::vector<float> inputs;
	const size_t readings = host_.readingCountAt(device_);
	inputs.reserve(readings + kRowFloats);
	for (size_t i = 0; i < readings; ++i) {
		inputs.push_back(host_.readingAt(device_, i));
	}
	for (size_t i = 0; i < kRowFloats; ++i) {
		inputs.push_back(field(i));
	}
	if (inputs != lastInputs_) {
		lastInputs_ = std::move(inputs);
		++serial_;
	}
	return serial_;
}

bool DeviceSpectrumSource::readFrame(SpectralPlotFrame& out, uint32_t& serial) const {
	serial = frameSerial();
	makeFrame(out);
	return true;
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

bool DeviceSpectrumSource::sidechainSounding() const {
	return host_.readingCountAt(device_) >= kReadingCount && host_.readingAt(device_, kReadingSideSounding) >= 0.5f;
}

bool DeviceSpectrumSource::vocoding() const {
	return field(static_cast<size_t>(Param::VocodeOn)) >= 0.5f && master() && sidechainSounding();
}

int DeviceSpectrumSource::overlay(float* hz, float* db, int max) const {
	if (!vocoding()) {
		return 0;
	}
	const int count = std::min(max, kSpectrumBands);
	for (int b = 0; b < count; ++b) {
		hz[b] = static_cast<float>(bandCentreHz(b));
		db[b] = host_.readingAt(device_, kReadingSideEnvelope + static_cast<size_t>(b));
	}
	return count;
}

} // namespace feathers::fx::spectral
