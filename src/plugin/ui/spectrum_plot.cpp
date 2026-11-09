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

#include "plugin/ui/spectrum_plot.h"
#include "plugin/ui/spectrum_axes.h"

#include "fx/eq/eq_readout.h"
#include "dsp/vox_anchors.h"
#include "dsp/osc_voice.h"
#include "plugin/spectral_state.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {

using C = SpectralControl;

constexpr float kRounding = 8.0f;
/// The terrain's bars: darker than the surface's lines, so the result's bars read over them.
constexpr uint32_t kTerrainBar = 0xff2c3552;
constexpr std::array<double, 8> kGridHz { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 };
constexpr std::array<int, 13> kHarmonicLabels { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64, 128, 256 };
constexpr std::array<const char* const, 5> kModeShort { "LP", "HP", "BP", "Notch", "Bell" };
/// THE SECOND FILTER'S HANDLE COLOUR (F2): the gamma axis's violet (tokens.h), apart from filter 1's value colour and the
/// operators' accent.
constexpr uint32_t kFilter2Colour = 0xffA78FD9;
static_assert(kModeShort.size() == kSpectralFilterModeNames.size());

std::string hzText(double hz) {
	char text[32];
	if (hz >= 1000.0) {
		std::snprintf(text, sizeof(text), hz >= 10000.0 ? "%.0f kHz" : "%.1f kHz", hz / 1000.0);
	}
	else {
		std::snprintf(text, sizeof(text), "%.0f Hz", hz);
	}
	return text;
}

std::string harmonicText(double k) {
	char text[32];
	std::snprintf(text, sizeof(text), "\xc3\x97%.1f", k); // ×
	return text;
}

} // namespace

SpectrumPlot::SpectrumPlot(ParamBridge& bridge)
    : ownedSource_(std::make_unique<BridgeSpectrumSource>(bridge, nullptr)), source_(*ownedSource_),
      staleThrough_(source_.frameSerial()) {
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { drag(e.position); };
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

SpectrumPlot::SpectrumPlot(SpectrumSource& source) : source_(source), staleThrough_(source.frameSerial()) {
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { drag(e.position); };
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

SpectrumPlot::~SpectrumPlot() {
	release(); // a gesture open at teardown closes, so the host is never left holding one
}

// ---- the mappings ------------------------------------------------------------------------------------------------

float SpectrumPlot::hzToX(double hz) const { return spectrum_axes::hzToX(hz, plotLeft(), plotRight()); }

double SpectrumPlot::xToHz(float x) const { return spectrum_axes::xToHz(x, plotLeft(), plotRight()); }

float SpectrumPlot::dbToY(double db) const { return spectrum_axes::dbToY(db, plotTop(), plotBottom()); }

double SpectrumPlot::yToDb(float y) const { return spectrum_axes::yToDb(y, plotTop(), plotBottom()); }

float SpectrumPlot::curveGainAtY(float y) const {
	const double db = yToDb(y);
	if (!(db > kCurveFloorDb)) {
		return 0.0f; // THE FLOOR IS SILENCE, exactly: a band can be drawn out
	}
	return std::min(kSpectralCurveGainMax, static_cast<float>(std::pow(10.0, db / 20.0)));
}

double SpectrumPlot::f0() const {
	return frame_.count > 0 && frame_.f0 > 0.0f ? static_cast<double>(frame_.f0) : kRestF0;
}

// ---- what is drawn -----------------------------------------------------------------------------------------------

SpectrumPlot::View SpectrumPlot::viewNow(size_t slot, bool live, bool draw) const {
	View v;
	v.slot = slot;
	v.live = live;
	v.master = source_.master();
	v.draw = draw;
	for (size_t c = 0; c < v.value.size(); ++c) {
		v.value[c] = source_.value(slot, static_cast<SpectralControl>(c));
	}
	v.curve = source_.curve(slot);
	return v;
}

bool SpectrumPlot::on(SpectralControl c) const {
	return steppedIndex(value(c), kEnableNames.size()) != 0;
}

bool SpectrumPlot::sync(size_t selected, bool live, bool draw) {
	bool moved = false;
	// A NEW ENGINE FRAME, keyed by its serial (never an address): the copy happens only when the serial moved, and only for
	// a frame published AFTER the plot was built (a panel is built fresh on every open) — an older one is a note that
	// may have ended long ago. Compared as a signed distance, so the serial's wrap cannot turn new into old.
	const uint32_t serial = source_.frameSerial();
	if (serial != shownSerial_ && static_cast<int32_t>(serial - staleThrough_) > 0) {
		SpectralPlotFrame next;
		uint32_t got = 0;
		if (source_.readFrame(next, got) && static_cast<int32_t>(got - staleThrough_) > 0) {
			frame_ = next;
			shownSerial_ = got;
			moved = true;
		}
	}
	const View now = viewNow(selected, live, draw);
	if (!primed_ || !(now == view_)) {
		view_ = now;
		primed_ = true;
		moved = true;
	}
	if (moved) {
		++redraws_;
		redraw();
	}
	return moved;
}

double SpectrumPlot::trackedCutoffHz(int filter) const {
	const double cutoff = std::exp2(value(spectralFilterControl(filter, C::FilterCutoff)));
	const double track = std::clamp(value(spectralFilterControl(filter, C::FilterKeyTrack)), 0.0, 1.0);
	return std::max(1.0, cutoff * std::pow(f0() / spectral::kKeyTrackPivotHz, track));
}

bool SpectrumPlot::bell(int filter) const {
	return steppedIndex(value(spectralFilterControl(filter, C::FilterMode)), kSpectralFilterModeNames.size())
	       == static_cast<size_t>(spectral::FilterMode::Bell);
}

std::array<SpectrumPlot::Handle, static_cast<size_t>(SpectrumPlot::Handle::kCount)> SpectrumPlot::handleOrder() const {
	std::array<Handle, static_cast<size_t>(Handle::kCount)> order {};
	const Handle selected = source_.selectedFilter() == 1 ? Handle::Filter2 : Handle::Filter;
	size_t n = 0;
	for (size_t i = 0; i < order.size(); ++i) {
		if (static_cast<Handle>(i) != selected) {
			order[n++] = static_cast<Handle>(i);
		}
	}
	order[n] = selected;
	return order;
}

double SpectrumPlot::shiftAnchorHz(bool high) const {
	if (on(C::FormantShiftLink)) {
		return kShiftAnchorHz;
	}
	const double crossover = std::exp2(value(C::FormantShiftCrossover));
	return high ? crossover * 2.0 : crossover * 0.5;
}

std::optional<double> SpectrumPlot::cutHz(double count) const {
	// A HARMONIC SPECTRUM, built once: ratio k at partial k − 1. pitchPartials reads only the ratios.
	static const partials::PartialSpectrum kHarmonic = [] {
		partials::PartialSpectrum s {};
		for (int i = 0; i < partials::kMaxPartials; ++i) {
			s.ratio[static_cast<size_t>(i)] = static_cast<float>(i + 1);
		}
		return s;
	}();
	// A rate far above any note's partials, so Nyquist cuts nothing: the line is the count's, not the band's.
	constexpr double kRate = 1.0e7;
	spectral::ShaperParams q;
	q.partialCount = { true, static_cast<float>(count) };
	q.stretch = { on(C::StretchOn), static_cast<float>(spectral::StretchOp::bOf(value(C::StretchAmount))) };
	q.shift = { on(C::ShiftOn), static_cast<float>(value(C::ShiftHz)) };
	alignas(16) std::array<float, partials::kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	const PartialPitches p =
	    pitchPartials(kHarmonic, q, f0(), kRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
	if (p.count <= 0) {
		return std::nullopt;
	}
	const double last = static_cast<double>(p.ratio[p.count - 1]);
	const double next = p.count < partials::kMaxPartials ? static_cast<double>(p.ratio[p.count]) : last + 1.0;
	return f0() * 0.5 * (last + next);
}

float SpectrumPlot::vowelToX(double position) const {
	constexpr double kLast = static_cast<double>(kVoxAnchorCount - 1);
	return plotLeft() + static_cast<float>(std::clamp(position, 0.0, kLast) / kLast) * (plotRight() - plotLeft());
}

double SpectrumPlot::xToVowel(float x) const {
	constexpr double kLast = static_cast<double>(kVoxAnchorCount - 1);
	const float span = plotRight() - plotLeft();
	return span > 0.0f ? std::clamp(static_cast<double>((x - plotLeft()) / span) * kLast, 0.0, kLast) : 0.0;
}

std::optional<visage::Point> SpectrumPlot::handleAt(Handle h) const {
	if (!view_.live || !view_.master) {
		return std::nullopt;
	}
	const auto at = [this](double hz, double db) { return visage::Point { hzToX(hz), dbToY(db) }; };
	// THE ONE INERTNESS RULE the row and the engine's blend read too (spectralControlInert, params.h).
	const bool linked = on(C::FormantShiftLink);
	const bool vowelSilent = !(value(C::FormantDepth) > 0.0);
	// EACH FILTER'S CONTROLS BY ITS OWN MODE AND OPEN END (spectralFilterOf); the rest ignore both.
	const auto inert = [&](SpectralControl c) {
		const int f = std::max(0, spectralFilterOf(c));
		return spectralControlInert(c, steppedIndex(value(spectralFilterControl(f, C::FilterMode)), kSpectralFilterModeNames.size()),
		                            linked, vowelSilent, source_.filterOpen(view_.slot, f)); // at its open end a filter is off (Z7d)
	};
	switch (h) {
	case Handle::Filter:
	case Handle::Filter2: {
		const int f = h == Handle::Filter2 ? 1 : 0;
		if (!source_.has(spectralFilterControl(f, C::FilterOn)) || !on(spectralFilterControl(f, C::FilterOn))) {
			return std::nullopt;
		}
		// A BELL'S HEIGHT IS ITS GAIN (0 dB at the corner's resting height); every other mode's is its Resonance.
		if (bell(f)) {
			return at(trackedCutoffHz(f), kFilterBaseDb + value(spectralFilterControl(f, C::FilterGain)) * kBellDbScale);
		}
		const SpectralControl res = spectralFilterControl(f, C::FilterResonance);
		const double resonance = inert(res) ? 0.0 : value(res);
		return at(trackedCutoffHz(f), kFilterBaseDb + resonance * kFilterSpanDb);
	}
	case Handle::Comb:
		if (!on(C::CombOn)) {
			return std::nullopt;
		}
		return at(f0() * value(C::CombSpacing), -value(C::CombDepth) * kCombSpanDb);
	case Handle::ShiftLow:
		if (!on(C::FormantShiftOn)) {
			return std::nullopt;
		}
		return at(shiftAnchorHz(false) * std::exp2(value(C::FormantShiftLow) / 12.0), kShiftDb);
	case Handle::ShiftHigh:
		if (!on(C::FormantShiftOn) || inert(C::FormantShiftHigh)) {
			return std::nullopt;
		}
		return at(shiftAnchorHz(true) * std::exp2(value(C::FormantShiftHigh) / 12.0), kShiftDb);
	case Handle::Crossover:
		if (!on(C::FormantShiftOn) || inert(C::FormantShiftCrossover)) {
			return std::nullopt;
		}
		return at(std::exp2(value(C::FormantShiftCrossover)), kCrossoverDb);
	case Handle::Tilt:
		if (!on(C::TiltOn)) {
			return std::nullopt;
		}
		return tiltGrip(value(C::TiltAmount));
	case Handle::Vowel:
		if (!on(C::FormantOn)) {
			return std::nullopt;
		}
		return visage::Point { vowelToX(value(C::FormantVowel)), dbToY(kVowelBaseDb + value(C::FormantDepth) * kVowelSpanDb) };
	case Handle::ShiftMarker:
		if (!on(C::ShiftOn)) {
			return std::nullopt;
		}
		return at(std::clamp(f0() + value(C::ShiftHz), kMinHz, kMaxHz), kShiftMarkerDb);
	case Handle::PartialsCut: {
		// IN PEAKS (OH) the kept partials are the loudest, not the lowest: there is no one frequency to cut at, so no line (the
		// cell's box sets the count, and the bars show which partials are kept).
		// AND IN CENTERS (OI) the kept are a set, not a prefix: only Harmonics has a cut.
		const bool peaks = source_.has(C::PartialsMode)
		                   && steppedIndex(value(C::PartialsMode), kSpectralPartialsModeNames.size())
		                          != static_cast<size_t>(spectral::PartialsScheme::Harmonics);
		if (!on(C::PartialsOn) || peaks) {
			return std::nullopt;
		}
		// BETWEEN THE LAST PARTIAL KEPT AND THE FIRST CUT, at their MOVED ratios (Stretch and Shift move them; cutHz).
		// A FRACTIONAL COUNT stands between its two whole counts' cuts, in log-Hz by its fraction (the drag's inverse).
		const double count = value(C::PartialsCount);
		const double whole = std::floor(count);
		const auto lower = cutHz(whole);
		if (!lower.has_value()) {
			return std::nullopt;
		}
		double hz = *lower;
		if (count > whole) {
			if (const auto upper = cutHz(whole + 1.0); upper.has_value() && *upper > *lower) {
				hz = *lower * std::exp2((count - whole) * std::log2(*upper / *lower));
			}
		}
		return at(std::clamp(hz, kMinHz, kMaxHz), kCutGripDb);
	}
	case Handle::kCount: break;
	}
	return std::nullopt;
}

std::optional<visage::Point> SpectrumPlot::handleForTest(Handle h) const {
	return handleAt(h);
}

bool SpectrumPlot::vertical(Handle h) const {
	switch (h) {
	case Handle::Filter:
	case Handle::Filter2: {
		// UP IS A BELL'S GAIN (never inert in Bell), or another mode's Resonance where it acts.
		const int f = h == Handle::Filter2 ? 1 : 0;
		if (bell(f)) {
			return true;
		}
		return !spectralControlInert(spectralFilterControl(f, C::FilterResonance),
		                             steppedIndex(value(spectralFilterControl(f, C::FilterMode)), kSpectralFilterModeNames.size()),
		                             on(C::FormantShiftLink), false, source_.filterOpen(view_.slot, f));
	}
	case Handle::Comb:
	case Handle::Tilt:
	case Handle::Vowel: return true; // up is Depth, which is never inert
	default: return false;
	}
}

int SpectrumPlot::tiltLine(double dbPerOctave, float* out) const {
	const int count = std::clamp(frame_.count, 0, SpectralPlotFrame::kPartials);
	if (count == 0) {
		return 0;
	}
	// THE ENGINE'S OWN ARITHMETIC: a drawer whose only operator is this Tilt, through the shaper's batch gains() over the
	// frame's terrain — so the line carries Tilt's level compensation exactly as a voice applies it (spectral_shaper.h:
	// out_i = T_i·c, c holding the terrain's power). k is the harmonic index (the shaper's contract), hz the partial's.
	spectral::ShaperParams p;
	p.tilt.on = true;
	p.tilt.dbPerOctave = static_cast<float>(dbPerOctave);
	const double base = f0();
	const spectral::PreparedShaper prepared = spectral::prepare(p, base);
	std::array<float, SpectralPlotFrame::kPartials> k {};
	std::array<float, SpectralPlotFrame::kPartials> hz {};
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		k[u] = static_cast<float>(i + 1);
		hz[u] = static_cast<float>(base * static_cast<double>(frame_.ratio[u]));
	}
	spectral::gains(prepared, k.data(), hz.data(), frame_.terrain.data(), count, out);
	return count;
}

float SpectrumPlot::lineY(float gain) const {
	return gain > 0.0f ? dbToY(std::max(kBottomDb, 20.0 * std::log10(static_cast<double>(gain)))) : plotBottom();
}

std::optional<visage::Point> SpectrumPlot::tiltEnd(const float* line, int count, bool clamped) const {
	// THE LINE'S RIGHT END: the highest partial on the axis — and only with a second one there, since a one-point line's end
	// does not move with the amount (the compensation holds its level), so a grip on it would set nothing.
	const double base = f0();
	int onAxis = 0;
	std::optional<visage::Point> end;
	for (int i = count - 1; i >= 0; --i) {
		const double hz = base * static_cast<double>(frame_.ratio[static_cast<size_t>(i)]);
		if (hz <= kMaxHz && hz >= kMinHz) {
			if (!end.has_value()) {
				const float y = lineY(line[static_cast<size_t>(i)]);
				end = visage::Point { hzToX(hz), clamped ? std::clamp(y, plotTop(), plotBottom()) : y };
			}
			if (++onAxis >= 2) {
				return end;
			}
		}
	}
	return std::nullopt; // no frame yet, or one partial: nothing to draw the line over
}

std::optional<visage::Point> SpectrumPlot::tiltGrip(double dbPerOctave, bool clamped) const {
	std::array<float, SpectralPlotFrame::kPartials> line {};
	const int count = tiltLine(dbPerOctave, line.data());
	return tiltEnd(line.data(), count, clamped);
}

std::string SpectrumPlot::noteLabel() const {
	if (frame_.count <= 0 || !std::isfinite(frame_.f0) || !(frame_.f0 > 0.0f)) {
		return {};
	}
	const std::string name = fx::eq::noteName(fx::eq::noteOf(static_cast<double>(frame_.f0)).midi);
	return frame_.playing != 0 ? name : "reference \xc2\xb7 " + name; // ·
}

std::string SpectrumPlot::label(Handle h) const {
	char text[48];
	switch (h) {
	case Handle::Filter:
	case Handle::Filter2: {
		const int f = h == Handle::Filter2 ? 1 : 0;
		const size_t mode = steppedIndex(value(spectralFilterControl(f, C::FilterMode)), kSpectralFilterModeNames.size());
		// KEY TRACK ON, THE CUTOFF FOLLOWS THE NOTE, so it reads as harmonics of it; off, it stays put in Hz.
		const double hz = trackedCutoffHz(f);
		const std::string where = value(spectralFilterControl(f, C::FilterKeyTrack)) > 0.0 ? harmonicText(hz / f0()) : hzText(hz);
		std::string text = (f == 1 ? "2: " : "") + std::string(kModeShort[mode]) + " " + where;
		if (bell(f)) {
			char db[24];
			std::snprintf(db, sizeof(db), " %+.1f dB", value(spectralFilterControl(f, C::FilterGain)));
			text += db;
		}
		return text;
	}
	case Handle::Comb: return "comb " + harmonicText(value(C::CombSpacing));
	case Handle::ShiftLow:
		std::snprintf(text, sizeof(text), "%+.0f st", value(C::FormantShiftLow));
		return text;
	case Handle::ShiftHigh:
		std::snprintf(text, sizeof(text), "%+.0f st", value(C::FormantShiftHigh));
		return text;
	case Handle::Crossover: return hzText(std::exp2(value(C::FormantShiftCrossover)));
	case Handle::Tilt:
		std::snprintf(text, sizeof(text), "%+.1f dB/oct", value(C::TiltAmount));
		return text;
	case Handle::Vowel: {
		// THE NEAREST VOWEL'S NAME and where between the anchors it stands.
		const double position = std::clamp(value(C::FormantVowel), 0.0, double(kVoxAnchorCount - 1));
		std::snprintf(text, sizeof(text), "%s %.1f", kVoxAnchorNames[static_cast<size_t>(std::lround(position))], position);
		return text;
	}
	case Handle::ShiftMarker:
		std::snprintf(text, sizeof(text), "%+.1f Hz", value(C::ShiftHz)); // the knob's precision (ParamUnit::Hertz)
		return text;
	case Handle::PartialsCut:
		std::snprintf(text, sizeof(text), "%.1f partials", value(C::PartialsCount));
		return text;
	case Handle::kCount: break;
	}
	return {};
}

std::string SpectrumPlot::labelForTest(Handle h) const {
	return label(h);
}

std::optional<size_t> SpectrumPlot::bandAt(float x) const {
	const double k = xToHz(x) / f0();
	if (!std::isfinite(k) || !(k >= 0.5)) {
		return std::nullopt;
	}
	// Held to the last band before rounding, so a huge k (a tiny f0) never reaches lround out of range.
	const long b = std::lround(std::min(k, static_cast<double>(spectral::kCurveBands))) - 1;
	return static_cast<size_t>(std::clamp<long>(b, 0, spectral::kCurveBands - 1));
}

bool SpectrumPlot::canDraw() const {
	// THE CURVE NEED NOT BE ON: a stroke switches it on (press), as the sketch's does.
	return view_.draw && view_.live && view_.master;
}

// ---- the hand ----------------------------------------------------------------------------------------------------

void SpectrumPlot::press(visage::Point p) {
	release();
	if (canDraw()) {
		gesture_.kind = Gesture::Kind::Stroke;
		gesture_.slot = view_.slot;
		gesture_.before = source_.curve(view_.slot);
		gesture_.last = p;
		// A STROKE ON AN OFF CURVE SWITCHES IT ON — inside the stroke's own step, since `before` holds the switch too, so one
		// undo takes back both the bands and the switch.
		source_.setCurveOn(view_.slot, true);
		stroke(p);
		return;
	}
	// THE NEAREST HANDLE IN REACH, THE SELECTED FILTER'S FIRST: within reach it wins outright (it is drawn in front, so it is
	// the one under the hand where the two filters' handles overlap). The Draw tool off, a press anywhere else does nothing.
	std::optional<Handle> grabbed;
	float best = kGrab;
	const Handle selectedFilter = source_.selectedFilter() == 1 ? Handle::Filter2 : Handle::Filter;
	if (const auto at = handleAt(selectedFilter); at.has_value() && std::hypot(at->x - p.x, at->y - p.y) <= kGrab) {
		grabbed = selectedFilter;
		gesture_.offset = { at->x - p.x, at->y - p.y };
	}
	for (const Handle h : handleOrder()) {
		if (grabbed.has_value() && *grabbed == selectedFilter) {
			break;
		}
		if (const auto at = handleAt(h); at.has_value()) {
			const float d = std::hypot(at->x - p.x, at->y - p.y);
			if (d <= best) {
				best = d;
				grabbed = h;
				gesture_.offset = { at->x - p.x, at->y - p.y };
			}
		}
	}
	if (!grabbed.has_value()) {
		return;
	}
	// WHICH CONTROL EACH AXIS WRITES. Nothing is opened here: a gesture opens on the first value that moves its control
	// (writeAxis), as a knob's does, so a click that moves nothing leaves the host nothing to undo.
	gesture_.kind = Gesture::Kind::Handle;
	gesture_.handle = *grabbed;
	gesture_.slot = view_.slot;
	switch (*grabbed) {
	case Handle::Filter:
	case Handle::Filter2: {
		const int f = *grabbed == Handle::Filter2 ? 1 : 0;
		source_.selectFilter(f); // GRABBING A FILTER'S HANDLE SELECTS ITS TAB (the row follows at its next sync)
		gesture_.xControl = spectralFilterControl(f, C::FilterCutoff);
		// UNDER NOTCH RESONANCE CHANGES NOTHING (the honesty rule), so the corner's height is not a control there; a Bell's is Gain.
		if (vertical(*grabbed)) {
			gesture_.yControl = spectralFilterControl(f, bell(f) ? C::FilterGain : C::FilterResonance);
		}
		break;
	}
	case Handle::Comb:
		gesture_.xControl = C::CombSpacing;
		gesture_.yControl = C::CombDepth;
		break;
	case Handle::ShiftLow: gesture_.xControl = C::FormantShiftLow; break;
	case Handle::ShiftHigh: gesture_.xControl = C::FormantShiftHigh; break;
	case Handle::Crossover: gesture_.xControl = C::FormantShiftCrossover; break;
	case Handle::Tilt: gesture_.yControl = C::TiltAmount; break;
	case Handle::Vowel:
		// ACROSS IS THE VOWEL, UP IS DEPTH. At Depth 0 the vowel is not heard (spectralControlInert, the row greys it), so
		// across moves nothing there — decided on EVERY move (moveHandle), since the drag's own height can bring Depth up.
		gesture_.xControl = C::FormantVowel;
		gesture_.yControl = C::FormantDepth;
		break;
	case Handle::ShiftMarker: gesture_.xControl = C::ShiftHz; break;
	case Handle::PartialsCut: gesture_.xControl = C::PartialsCount; break;
	case Handle::kCount: break;
	}
}

void SpectrumPlot::writeAxis(std::unique_ptr<device::Modulatable>& control, SpectralControl c, double value) {
	if (c == C::kCount) {
		return;
	}
	if (!control) {
		double lo = 0.0;
		double hi = 0.0;
		if (!source_.range(gesture_.slot, c, lo, hi) || std::clamp(value, lo, hi) == source_.value(gesture_.slot, c)) {
			return; // not moved yet: no gesture
		}
		control = source_.control(gesture_.slot, c);
		control->beginGesture();
	}
	control->setValue(value);
}

void SpectrumPlot::drag(visage::Point p) {
	if (gesture_.kind == Gesture::Kind::Stroke) {
		stroke(p);
	}
	else if (gesture_.kind == Gesture::Kind::Handle) {
		moveHandle(p);
	}
}

void SpectrumPlot::release() {
	if (gesture_.kind == Gesture::Kind::Stroke) {
		// ONE STROKE, ONE STEP: the curve before the press against the curve now, and nothing for a stroke that moved
		// nothing.
		const spectral::DrawnCurve after = source_.curve(gesture_.slot);
		if (history_ != nullptr && !(after == gesture_.before)) {
			history_->record(
			    std::make_unique<CurveEdit>(source_.curveWriter(gesture_.slot), gesture_.slot, gesture_.before, after, "Draw"));
		}
	}
	if (gesture_.x) {
		gesture_.x->endGesture();
	}
	if (gesture_.y) {
		gesture_.y->endGesture();
	}
	gesture_ = Gesture {};
}

void SpectrumPlot::stroke(visage::Point p) {
	const float xa = gesture_.last.x;
	const float ya = gesture_.last.y;
	const auto from = bandAt(std::min(xa, p.x));
	const auto to = bandAt(std::max(xa, p.x));
	gesture_.last = p;
	if (!to.has_value()) {
		return; // all of it below the fundamental
	}
	const size_t lo = from.value_or(0);
	const double base = f0();
	for (size_t b = lo; b <= *to; ++b) {
		// THE HEIGHT THE STROKE CROSSES THIS BAND AT: the line from the last point to this one, read at the band's
		// harmonic (clamped to the segment), so a fast hand that skips bands still paints every one it passed.
		const float xb = hzToX(base * static_cast<double>(b + 1));
		const float span = p.x - xa;
		const float t = std::abs(span) < 1e-6f ? 1.0f : std::clamp((xb - xa) / span, 0.0f, 1.0f);
		const float y = ya + (p.y - ya) * t;
		source_.setCurveBand(gesture_.slot, b, curveGainAtY(y));
	}
}

void SpectrumPlot::moveHandle(visage::Point pointer) {
	// WHERE THE HANDLE IS, not where the pointer is: the press's offset from the handle's centre rides along.
	const visage::Point p { pointer.x + gesture_.offset.x, pointer.y + gesture_.offset.y };
	const double hz = std::clamp(xToHz(p.x), kMinHz, kMaxHz);
	const double db = yToDb(p.y);
	double x = 0.0;
	double y = 0.0;
	switch (gesture_.handle) {
	case Handle::Filter:
	case Handle::Filter2: {
		// THE POINTER IS THE TRACKED CORNER: the param is the cutoff before Key Track moves it.
		const int f = gesture_.handle == Handle::Filter2 ? 1 : 0;
		const double track = std::clamp(value(spectralFilterControl(f, C::FilterKeyTrack)), 0.0, 1.0);
		x = std::log2(hz) - track * std::log2(f0() / spectral::kKeyTrackPivotHz);
		y = gesture_.yControl == spectralFilterControl(f, C::FilterGain) ? (db - kFilterBaseDb) / kBellDbScale
		                                                                  : (db - kFilterBaseDb) / kFilterSpanDb;
		break;
	}
	case Handle::Comb:
		x = hz / f0();
		y = -db / kCombSpanDb;
		break;
	case Handle::ShiftLow: x = 12.0 * std::log2(hz / shiftAnchorHz(false)); break;
	case Handle::ShiftHigh: x = 12.0 * std::log2(hz / shiftAnchorHz(true)); break;
	case Handle::Crossover: x = std::log2(hz); break;
	case Handle::Tilt: {
		// THE GRIP RIDES THE LINE'S RIGHT END, so the amount is the one whose compensated line ends at the pointer's
		// height. That end rises with the amount, so a bisection over the param's range finds it (to well under a pixel).
		double lo = -24.0;
		double hi = 24.0;
		source_.range(gesture_.slot, C::TiltAmount, lo, hi);
		const auto endY = [this](double amount) {
			const auto grip = tiltGrip(amount, false);
			return grip.has_value() ? grip->y : 0.0f;
		};
		for (int i = 0; i < 40; ++i) {
			const double mid = 0.5 * (lo + hi);
			(endY(mid) > p.y ? lo : hi) = mid; // y grows downward: a grip below the pointer needs more amount
		}
		y = 0.5 * (lo + hi);
		break;
	}
	case Handle::Vowel:
		x = xToVowel(p.x);
		y = (db - kVowelBaseDb) / kVowelSpanDb;
		break;
	case Handle::ShiftMarker:
		// THE MARKER IS THE FUNDAMENTAL MOVED: the shift is where it stands less the note. A log axis cannot show 0 Hz or
		// below, so a drag reaches down to just above −f0; the knob reaches the rest of the range.
		x = xToHz(p.x) - f0();
		break;
	case Handle::PartialsCut: {
		// THE COUNT WHOSE CUT STANDS NEAREST THE POINTER: the cut moves monotonically with the count, so a bisection over the
		// control's whole counts finds the first at or past the pointer, and the nearer of it and the one before wins.
		double countLo = 8.0;
		double countHi = 320.0;
		source_.range(gesture_.slot, C::PartialsCount, countLo, countHi);
		int lo = static_cast<int>(std::ceil(countLo));
		int hi = static_cast<int>(countHi);
		const auto cutAt = [this](int n) { return cutHz(static_cast<double>(n)).value_or(kMinHz); };
		int a = lo, z = hi;
		while (a < z) {
			const int mid = (a + z) / 2;
			if (cutAt(mid) < hz) {
				a = mid + 1;
			}
			else {
				z = mid;
			}
		}
		// AND BETWEEN TWO WHOLE COUNTS, by the pointer's log-Hz between their cuts: the count is continuous, and so is the
		// drag (the line is drawn by the same interpolation, handleAt).
		const int n = std::max(a, lo + 1);
		const double below = cutAt(n - 1);
		const double above = cutAt(n);
		const double t = above > below ? std::log2(hz / below) / std::log2(above / below) : 0.0;
		x = std::clamp(static_cast<double>(n - 1) + std::clamp(t, 0.0, 1.0), static_cast<double>(lo), static_cast<double>(hi));
		break;
	}
	case Handle::kCount: break;
	}
	if (gesture_.handle == Handle::Vowel) {
		// DEPTH FIRST, THEN THE VOWEL ONLY WHILE IT IS HEARD: the inertness is read after this move's Depth lands, so a drag
		// that lifts Depth from 0 brings the vowel's axis alive within the same drag, and one that lowers it to 0 stops it.
		writeAxis(gesture_.y, gesture_.yControl, y);
		const double depthNow = source_.value(gesture_.slot, C::FormantDepth);
		// AND ONLY ONCE THE HANDLE HAS MOVED ACROSS: the walk's even layout does not round-trip a position to the bit, so a
		// drag straight up would otherwise open the Vowel's gesture for a value it did not mean to change.
		const bool across = std::abs(vowelToX(value(C::FormantVowel)) - p.x) >= 0.5f;
		if (across && !spectralControlInert(C::FormantVowel, 0, false, !(depthNow > 0.0))) {
			writeAxis(gesture_.x, gesture_.xControl, x);
		}
	}
	else {
		writeAxis(gesture_.x, gesture_.xControl, x);
		writeAxis(gesture_.y, gesture_.yControl, y);
	}
	// The view follows the params at the next sync; read it now too, so a drag's next frame starts from where it is.
	view_ = viewNow(view_.slot, view_.live, view_.draw);
	// AND REPAINT HERE: with the view taken above, the next sync sees nothing new, and a drag that changes nothing heard
	// (the crossover with the shifts equal, the comb at depth 0) would leave the handle and its label frozen.
	++redraws_;
	redraw();
}

// ---- the picture -------------------------------------------------------------------------------------------------

void SpectrumPlot::draw(visage::Canvas& canvas) {
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kRounding);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), kRounding, 1.0f);
	drawGrid(canvas);
	drawBars(canvas);
	drawOverlay(canvas);
	drawCurve(canvas);
	drawHandles(canvas);
}

void SpectrumPlot::drawGrid(visage::Canvas& canvas) {
	const visage::Font& face = font(kKnobLabelSize);
	char text[16];
	for (const double hz : kGridHz) {
		const float x = hzToX(hz);
		canvas.setColor(withAlpha(kLine, 0.6f));
		canvas.fill(x, plotTop(), 1.0f, plotBottom() - plotTop());
		canvas.setColor(kDisabled);
		if (hz >= 1000.0) {
			std::snprintf(text, sizeof(text), "%.0fk", hz / 1000.0);
		}
		else {
			std::snprintf(text, sizeof(text), "%.0f", hz);
		}
		canvas.text(text, face, visage::Font::kLeft, x + 2.0f, plotBottom() + 1.0f, 40.0f, kPadBottom - 2.0f);
	}
	for (double db = 0.0; db >= kBottomDb; db -= 12.0) {
		const float y = dbToY(db);
		canvas.setColor(withAlpha(kLine, db == 0.0 ? 0.9f : 0.45f));
		canvas.fill(plotLeft(), y, plotRight() - plotLeft(), 1.0f);
		canvas.setColor(kDisabled);
		std::snprintf(text, sizeof(text), "%.0f", db);
		canvas.text(text, face, visage::Font::kRight, 0.0f, y - 6.0f, kPadLeft - 4.0f, 12.0f);
	}
	// THE NOTE'S HARMONIC NUMBERS along the top: the axis the comb and the curve are measured on.
	canvas.setColor(kMuted);
	const double base = f0();
	// A LABEL TOO CLOSE TO THE ONE BEFORE IS SKIPPED: on a compact plot (a device card's) the high harmonics crowd together; the
	// drawer's panel is wide enough that none ever is.
	float lastX = -1e9f;
	for (const int k : kHarmonicLabels) {
		const double hz = base * k;
		if (hz < kMinHz || hz > kMaxHz) {
			continue;
		}
		const float x = hzToX(hz);
		if (x - lastX < 18.0f) {
			continue;
		}
		lastX = x;
		std::snprintf(text, sizeof(text), "%d", k);
		canvas.text(text, face, visage::Font::kCenter, x - 12.0f, 2.0f, 24.0f, kPadTop - 4.0f);
	}
	// WHICH NOTE THIS IS: the playing note, or the reference the next note would get.
	if (const std::string note = noteLabel(); !note.empty()) {
		canvas.setColor(frame_.playing != 0 ? kInk : kMuted);
		canvas.text(note, face, visage::Font::kRight, plotRight() - 160.0f, plotTop() + 2.0f, 156.0f, 12.0f);
	}
}

void SpectrumPlot::drawOverlay(visage::Canvas& canvas) {
	// THE SOURCE'S LINE (a vocoder's sidechain envelope), its peak at the axis' 0 dB, as the bars are referred to theirs.
	constexpr int kMax = 128;
	std::array<float, kMax> hz {}, db {};
	const int count = std::min(source_.overlay(hz.data(), db.data(), kMax), kMax);
	if (count < 2) {
		return;
	}
	const float peak = *std::max_element(db.begin(), db.begin() + count);
	canvas.setColor(withAlpha(kInk, 0.8f));
	for (int i = 1; i < count; ++i) {
		const auto y = [&](int j) {
			return std::clamp(dbToY(std::max(kBottomDb, static_cast<double>(db[static_cast<size_t>(j)] - peak))), plotTop(), plotBottom());
		};
		canvas.segment(hzToX(hz[static_cast<size_t>(i - 1)]), y(i - 1), hzToX(hz[static_cast<size_t>(i)]), y(i), 1.5f, true);
	}
}

void SpectrumPlot::drawBars(visage::Canvas& canvas) {
	const int count = std::clamp(frame_.count, 0, SpectralPlotFrame::kPartials);
	float ref = 0.0f;
	for (int i = 0; i < count; ++i) {
		ref = std::max(ref, frame_.terrain[static_cast<size_t>(i)]);
	}
	if (!(ref > 0.0f)) {
		return;
	}
	const double base = f0();
	const float barW = std::clamp((hzToX(2.0 * base) - hzToX(base)) * 0.45f, 1.0f, 4.0f);
	const float foot = plotBottom();
	const auto bar = [&](double hz, float amplitude, float dx, float w) {
		if (!(amplitude > 0.0f)) {
			return;
		}
		const double db = std::max(kBottomDb, 20.0 * std::log10(static_cast<double>(amplitude / ref)));
		const float top = std::max(plotTop(), dbToY(db));
		canvas.fill(hzToX(hz) + dx - w * 0.5f, top, w, std::max(0.0f, foot - top));
	};
	const bool stereo = frame_.stereo != 0;
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		const double hz = base * static_cast<double>(frame_.ratio[u]);
		if (hz < kMinHz || hz > kMaxHz) {
			continue;
		}
		canvas.setColor(kTerrainBar);
		bar(hz, frame_.terrain[u], 0.0f, barW + 1.0f);
		if (stereo) {
			canvas.setColor(kModulationL);
			bar(hz, frame_.left[u], -barW * 0.35f, std::max(1.0f, barW * 0.6f));
			canvas.setColor(kModulationR);
			bar(hz, frame_.right[u], barW * 0.35f, std::max(1.0f, barW * 0.6f));
		}
		else {
			canvas.setColor(view_.master ? kValue : kMuted);
			bar(hz, frame_.left[u], 0.0f, std::max(1.0f, barW * 0.8f));
		}
	}
}

void SpectrumPlot::drawCurve(visage::Canvas& canvas) {
	// THE SELECTED SOUND'S CURVE, on while it is on, faint while only the Draw tool shows it.
	if (!view_.curve.on && !view_.draw) {
		return;
	}
	const auto yOf = [this](float g) { return std::max(plotTop(), lineY(g)); };
	canvas.setColor(withAlpha(kInk, view_.curve.on && view_.master && view_.live ? 0.85f : 0.35f));
	const double base = f0();
	float px = 0.0f;
	float py = 0.0f;
	for (size_t b = 0; b < spectral::kCurveBands; ++b) {
		const double hz = base * static_cast<double>(b + 1);
		if (hz > kMaxHz) {
			break;
		}
		const float x = hzToX(hz);
		const float y = yOf(view_.curve.gain[b]);
		if (b > 0) {
			canvas.segment(px, py, x, y, 1.5f, true);
		}
		canvas.circle(x - 1.5f, y - 1.5f, 3.0f);
		px = x;
		py = y;
	}
}

void SpectrumPlot::drawHandles(visage::Canvas& canvas) {
	const visage::Font& face = font(kKnobReadoutSize);
	// TILT'S LINE, compensated as the engine applies it (tiltLine), computed ONCE for this paint: the line and its grip
	// (the line's right end, tiltEnd) come from the one call.
	std::array<float, SpectralPlotFrame::kPartials> line {};
	std::optional<visage::Point> tiltAt;
	if (view_.live && view_.master && on(C::TiltOn)) { // handleAt's rule for Tilt, without computing the line twice
		const int count = tiltLine(value(C::TiltAmount), line.data());
		tiltAt = tiltEnd(line.data(), count, true);
		const double base = f0();
		canvas.setColor(withAlpha(kAccent, 0.6f));
		bool have = false;
		float px = 0.0f;
		float py = 0.0f;
		for (int i = 0; i < count; ++i) {
			const double hz = base * static_cast<double>(frame_.ratio[static_cast<size_t>(i)]);
			if (hz < kMinHz || hz > kMaxHz) {
				continue;
			}
			const float x = hzToX(hz);
			const float y = std::clamp(lineY(line[static_cast<size_t>(i)]), plotTop(), plotBottom());
			if (have) {
				canvas.segment(px, py, x, y, 1.5f, true);
			}
			have = true;
			px = x;
			py = y;
		}
	}
	for (const Handle h : handleOrder()) { // the selected filter's last: in front
		const auto at = h == Handle::Tilt ? tiltAt : handleAt(h);
		if (!at.has_value()) {
			continue;
		}
		const float x = at->x;
		const float y = at->y;
		const bool upDown = vertical(h);
		const uint32_t colour = h == Handle::Filter ? kValue : h == Handle::Filter2 ? kFilter2Colour : kAccent;
		canvas.setColor(kInk);
		switch (h) {
		case Handle::ShiftLow:
		case Handle::ShiftHigh: {
			// AN ARROW FROM ITS ANCHOR: the shift reads as the distance it has moved the envelope.
			const float anchor = hzToX(shiftAnchorHz(h == Handle::ShiftHigh));
			canvas.setColor(withAlpha(colour, 0.5f));
			canvas.segment(anchor, y, x, y, 1.0f, true);
			canvas.setColor(colour);
			const float dir = x >= anchor ? 1.0f : -1.0f;
			canvas.triangle(x + 6.0f * dir, y, x - 2.0f * dir, y - 5.0f, x - 2.0f * dir, y + 5.0f);
			break;
		}
		case Handle::Crossover:
			canvas.setColor(withAlpha(colour, 0.35f));
			canvas.fill(x, plotTop(), 1.0f, plotBottom() - plotTop());
			canvas.setColor(colour);
			canvas.roundedRectangle(x - 5.0f, y - 5.0f, 10.0f, 10.0f, 2.0f);
			break;
		case Handle::Comb:
			canvas.setColor(colour);
			canvas.triangle(x - 6.0f, y - 5.0f, x + 6.0f, y - 5.0f, x, y + 6.0f);
			break;
		case Handle::Vowel: {
			// THE TWO PEAKS WHERE THEY SOUND, as ticks at F1 and F2 (the walk lays the handle out evenly, the peaks sit in Hz),
			// then the handle: a ring, since it moves both ways.
			const VowelFormants v = vowelFormants(value(C::FormantVowel));
			canvas.setColor(withAlpha(colour, 0.45f));
			for (const double hz : { v.f1Hz, v.f2Hz }) {
				if (hz >= kMinHz && hz <= kMaxHz) {
					canvas.fill(hzToX(hz), plotTop(), 1.0f, plotBottom() - plotTop());
				}
			}
			canvas.setColor(colour);
			canvas.circle(x - 6.0f, y - 6.0f, 12.0f);
			canvas.setColor(kVoid);
			canvas.circle(x - 3.0f, y - 3.0f, 6.0f);
			break;
		}
		case Handle::ShiftMarker: {
			// FROM THE FUNDAMENTAL TO WHERE THE SHIFT PUTS IT, an arrow as Formant Shift's are.
			const float from = hzToX(std::clamp(f0(), kMinHz, kMaxHz));
			canvas.setColor(withAlpha(colour, 0.5f));
			canvas.segment(from, y, x, y, 1.0f, true);
			canvas.setColor(colour);
			canvas.roundedRectangle(x - 3.0f, y - 7.0f, 6.0f, 14.0f, 3.0f);
			break;
		}
		case Handle::PartialsCut:
			canvas.setColor(withAlpha(colour, 0.35f));
			canvas.fill(x, plotTop(), 1.0f, plotBottom() - plotTop());
			canvas.setColor(colour);
			canvas.roundedRectangle(x - 3.0f, y - 7.0f, 6.0f, 14.0f, 3.0f);
			break;
		default:
			// A horizontal-only handle is drawn as a bar, so its shape says which ways it moves.
			canvas.setColor(colour);
			if (upDown) {
				canvas.circle(x - 6.0f, y - 6.0f, 12.0f);
				canvas.setColor(kVoid);
				canvas.circle(x - 3.0f, y - 3.0f, 6.0f);
			}
			else {
				canvas.roundedRectangle(x - 3.0f, y - 7.0f, 6.0f, 14.0f, 3.0f);
			}
			break;
		}
		// A COMPACT PLOT (a device card's, narrower than kCompactWidth) labels only the handle in the hand: at that width every
		// handle's label at once runs into the next, and into the note's. The drawer's panel is never that narrow.
		if (width() < kCompactWidth && !(gesture_.kind == Gesture::Kind::Handle && gesture_.handle == h)) {
			continue;
		}
		canvas.setColor(colour);
		const bool leftOfHandle = x > plotRight() - 90.0f;
		canvas.text(label(h), face, leftOfHandle ? visage::Font::kRight : visage::Font::kLeft,
		            leftOfHandle ? x - 100.0f : x + 9.0f, y - 16.0f, 90.0f, 11.0f);
	}
}

} // namespace feathers::ui

namespace feathers::ui {

const char* SpectrumPlot::tooltipAt(visage::Point local) const {
	for (size_t i = 0; i < static_cast<size_t>(Handle::kCount); ++i) {
		const auto h = static_cast<Handle>(i);
		const auto at = handleAt(h);
		if (!at.has_value() || std::hypot(at->x - local.x, at->y - local.y) > kGrab) {
			continue;
		}
		using C = SpectralControl;
		switch (h) {
		case Handle::Filter: return source_.tooltip(view_.slot, C::FilterCutoff);
		case Handle::Filter2: return source_.tooltip(view_.slot, C::Filter2Cutoff);
		case Handle::Comb: return source_.tooltip(view_.slot, C::CombSpacing);
		case Handle::ShiftLow: return source_.tooltip(view_.slot, C::FormantShiftLow);
		case Handle::ShiftHigh: return source_.tooltip(view_.slot, C::FormantShiftHigh);
		case Handle::Crossover: return source_.tooltip(view_.slot, C::FormantShiftCrossover);
		case Handle::Tilt: return source_.tooltip(view_.slot, C::TiltAmount);
		case Handle::Vowel: return source_.tooltip(view_.slot, C::FormantVowel);
		case Handle::ShiftMarker: return source_.tooltip(view_.slot, C::ShiftHz);
		case Handle::PartialsCut: return source_.tooltip(view_.slot, C::PartialsCount);
		case Handle::kCount: break;
		}
	}
	return nullptr;
}

} // namespace feathers::ui
