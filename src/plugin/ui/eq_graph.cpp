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

#include "plugin/ui/eq_graph.h"

#include "fx/eq/eq_readout.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace eq = fx::eq;

namespace {

constexpr float kLabelSize = 8.0f;
/// A band outside the viewed path (step 6).
constexpr float kOutOfViewAlpha = 0.35f;
/// FilterCard's density (`filter_card.h:246`): two points per pixel, clamped — 96 points once missed a
/// resonant peak by 1.97 dB.
constexpr float kPointsPerPixel = 2.0f;
constexpr int kMinPoints = 601;
constexpr int kMaxPoints = 2048;

[[nodiscard]] uint32_t bandHue(int band) {
	return kSeedHues[static_cast<size_t>(band) % kSeedHues.size()];
}

[[nodiscard]] bool isGainType(eq::BandType t) {
	return eq::hasGain(t);
}

} // namespace

EqGraph::EqGraph(backplane::DeviceHost& host, DeviceInstanceId id, ControlFn control, bool compact)
    : host_(host), device_(id), control_(std::move(control)), compact_(compact) {
	for (auto* a : { &analyser_.pre, &analyser_.post, &analyser_.peak, &rawPre_, &rawPost_ }) {
		a->fill(eq::kAnalyserFloorDb);
	}
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const View v = baseView(); // THE HANDLES ARE ON THE BASE (MP): they stay where the hand left them
		const visage::Point p = e.position;
		// THE RAIL: an off band's marker. A press switches it on where it sits, and selects it.
		if (railBounds().contains(p)) {
			int best = -1;
			float bestD = kGrabRadius;
			const eq_plot::Geometry g = geometry();
			for (int b = 0; b < eq::kBandCount; ++b) {
				if (v.bands[static_cast<size_t>(b)].on) {
					continue;
				}
				const float d = std::fabs(g.xForHz(v.unclampedHz[static_cast<size_t>(b)]) - p.x);
				if (d < bestD) {
					bestD = d;
					best = b;
				}
			}
			if (best >= 0) {
				DeviceControl& on = control_(eq::bandIndex(best, eq::BandField::On));
				on.beginGesture();
				on.setValue(1.0);
				on.endGesture();
				select(best);
			}
			return;
		}
		// THE ANALYSER CHIP, panel only: its view menu.
		if (!compact_ && analyserChipBounds().contains(p)) {
			showAnalyserMenu(p);
			return;
		}
		// THE VIEW CHIP, panel only: Stereo / Mid / Side.
		if (!compact_ && viewChipBounds().contains(p)) {
			showViewMenu(p);
			return;
		}
		const int b = bandAt(p, v);
		if (b < 0) {
			// THE PIVOT, when there is a tilt for it to turn.
			if (field(eq::paramIndex(eq::Param::Tilt)) != 0.0f) { // the STORED tilt: the base view has none
				const visage::Point piv = pivotAt(v);
				if (std::hypot(piv.x - p.x, piv.y - p.y) < kGrabRadius) {
					drag_ = {};
					drag_.active = true;
					drag_.pivot = true;
					drag_.fine = e.isShiftDown();
					drag_.anchor = p;
					drag_.anchorFreqLog2 = field(eq::paramIndex(eq::Param::TiltPivot));
					control_(eq::paramIndex(eq::Param::TiltPivot)).beginGesture();
					return;
				}
			}
			// THE dB AXIS LABEL, panel only: the zoom.
			if (!compact_ && p.x < plotBounds().x() + 34.0f && p.y < plotBounds().y() + 14.0f) {
				showRangeMenu(p);
			}
			return;
		}
		if (e.shouldTriggerPopup()) {
			select(b);
			showTypeMenu(b, p);
			return;
		}
		if (e.repeat_click_count >= 2) {
			// DOUBLE-CLICK: the band off. (On again is a press on its rail marker.)
			DeviceControl& on = control_(eq::bandIndex(b, eq::BandField::On));
			on.beginGesture();
			on.setValue(0.0);
			on.endGesture();
			return;
		}
		select(b);
		beginDrag(b, p, e.isShiftDown());
	};
	onMouseDrag() += [this](const visage::MouseEvent& e) {
		if (drag_.active) {
			dragTo(e.position, e.isShiftDown());
		}
	};
	onMouseUp() += [this](const visage::MouseEvent&) { endDrag(); };
	onMouseMove() = [this](const visage::MouseEvent& e) {
		hover_ = e.position;
		hovering_ = true;
		redraw();
	};
	onMouseExit() = [this](const visage::MouseEvent&) {
		hovering_ = false;
		redraw();
	};
	// THE WHEEL OVER A HANDLE IS ITS Q — a tenth of an octave a notch, a fiftieth with Shift.
	onMouseWheel() = [this](const visage::MouseEvent& e) -> bool {
		const View v = baseView();
		const int b = bandAt(e.position, v);
		if (b < 0 || e.wheel_delta_y == 0.0f) {
			return false;
		}
		DeviceControl& q = control_(eq::bandIndex(b, eq::BandField::Q));
		const double step = (e.isShiftDown() ? 0.1 * kFineFactor : 0.1) * (e.wheel_delta_y > 0.0f ? 1.0 : -1.0);
		const device::ParamDesc& d = q.desc();
		q.beginGesture();
		q.setValue(std::clamp(q.value() + step, d.min, d.max));
		q.endGesture();
		redraw();
		return true;
	};
}

float EqGraph::field(size_t i) const {
	if (i < host_.fieldsPresentAt(device_)) {
		return host_.fieldAt(device_, i);
	}
	return i < eq::kParamCount ? static_cast<float>(eq::kParamDescs[i].def) : 0.0f;
}

float EqGraph::modulated(size_t i) const {
	if (i < host_.fieldsPresentAt(device_)) {
		return host_.modulatedFieldAt(device_, i);
	}
	return field(i);
}

double EqGraph::keyOctaves() const {
	// THE DEVICE'S OWN NUMBER (it publishes 0 with Key Track off); the stored switch gates it so Off draws at once.
	if (field(eq::paramIndex(eq::Param::KeyTrack)) < 0.5f || host_.readingCountAt(device_) < eq::kReadingCount) {
		return 0.0;
	}
	const float octaves = host_.readingAt(device_, static_cast<size_t>(eq::Reading::KeyOctaves));
	return std::isfinite(octaves) ? static_cast<double>(octaves) : 0.0;
}

int EqGraph::selected() const {
	return std::clamp(static_cast<int>(std::lround(field(eq::extraIndex(eq::Extra::Selected)))), 0,
	                  eq::kBandCount - 1);
}

void EqGraph::select(int band) {
	band = std::clamp(band, 0, eq::kBandCount - 1);
	if (band != selected()) {
		host_.setField(device_, eq::extraIndex(eq::Extra::Selected), static_cast<float>(band));
	}
	if (onSelected) {
		onSelected(band);
	}
	redraw();
}

float EqGraph::rangeDb() const {
	if (compact_) {
		return eq::kDbRanges[0];
	}
	const int i = std::clamp(static_cast<int>(std::lround(field(eq::extraIndex(eq::Extra::DbRange)))), 0,
	                         static_cast<int>(std::size(eq::kDbRanges)) - 1);
	return eq::kDbRanges[static_cast<size_t>(i)];
}

visage::Bounds EqGraph::plotBounds() const {
	return { 0.0f, 0.0f, width(), std::max(1.0f, height() - kRailHeight) };
}

visage::Bounds EqGraph::railBounds() const {
	return { 0.0f, std::max(0.0f, height() - kRailHeight), width(), kRailHeight };
}

eq_plot::Geometry EqGraph::geometry() const {
	const visage::Bounds b = plotBounds();
	return { b.x(), b.y(), b.width(), b.height(), rangeDb() };
}

EqGraph::View EqGraph::view() const {
	return viewOf(true);
}

EqGraph::View EqGraph::baseView() const {
	return viewOf(false);
}

EqGraph::View EqGraph::viewOf(bool live) const {
	// LIVE: where modulation left each control, the globals and Key Track's shift — what is heard. BASE (backlog MP,
	// shape B): the BANDS' stored values alone, with Offset, Amount and Tilt at identity, so nothing a global does —
	// stored, host-automated or modulated — moves a handle. (The pivot keeps its stored place: it is a handle too.)
	const auto at = [&](size_t i) { return live ? modulated(i) : field(i); };
	View v;
	const double rate = host_.sampleRate();
	v.sampleRate = rate > 0.0 ? rate : 48000.0;
	if (live) {
		v.globals.offsetOctaves = static_cast<double>(modulated(eq::paramIndex(eq::Param::Offset))) + keyOctaves();
		v.globals.amount = modulated(eq::paramIndex(eq::Param::Amount));
		v.globals.tiltDbPerOctave = modulated(eq::paramIndex(eq::Param::Tilt));
	}
	v.globals.tiltPivotHz = std::exp2(static_cast<double>(at(eq::paramIndex(eq::Param::TiltPivot))));
	for (int b = 0; b < eq::kBandCount; ++b) {
		eq::Band band;
		band.type = static_cast<eq::BandType>(std::clamp(
		    static_cast<int>(std::lround(field(eq::bandIndex(b, eq::BandField::Type)))), 0,
		    static_cast<int>(eq::BandType::kCount) - 1));
		band.on = field(eq::bandIndex(b, eq::BandField::On)) >= 0.5f;
		band.freqHz = std::exp2(static_cast<double>(at(eq::bandIndex(b, eq::BandField::Freq))));
		band.gainDb = at(eq::bandIndex(b, eq::BandField::Gain));
		band.q = std::exp2(static_cast<double>(at(eq::bandIndex(b, eq::BandField::Q))));
		band.slopeIndex = static_cast<int>(std::lround(field(eq::bandIndex(b, eq::BandField::Slope))));
		const auto k = static_cast<size_t>(b);
		v.unclampedHz[k] = band.freqHz * std::exp2(v.globals.offsetOctaves);
		v.bands[k] = eq::resolve(band, v.globals, v.sampleRate);
		v.designs[k] = eq::design(v.bands[k], v.sampleRate);
		v.channel[k] = eq::channelOf(field(eq::extraIndex(eq::channelField(b))));
	}
	v.tilt = eq::designTilt(v.globals, v.sampleRate);
	v.viewChannel = viewChannel();
	return v;
}

eq::Channel EqGraph::viewChannel() const {
	return eq::channelOf(field(eq::extraIndex(eq::Extra::AnalyserChannel)));
}

double EqGraph::pathDb(const View& v, eq::Channel path, double hz) {
	std::array<eq::BandDesign, eq::kBandCount> designs = v.designs;
	for (size_t b = 0; b < designs.size(); ++b) {
		if (!inPath(v.channel[b], path)) {
			designs[b] = eq::BandDesign {};
		}
	}
	return eq::responseDb(designs, v.tilt, hz, v.sampleRate);
}

bool EqGraph::sameResponse(const View& a, const View& b) {
	for (size_t k = 0; k < a.bands.size(); ++k) {
		const eq::EffectiveBand& x = a.bands[k];
		const eq::EffectiveBand& y = b.bands[k];
		if (x.type != y.type || x.on != y.on || x.freqHz != y.freqHz || x.gainDb != y.gainDb || x.q != y.q
		    || x.slopeIndex != y.slopeIndex) {
			return false;
		}
	}
	return a.globals.tiltDbPerOctave * a.globals.amount == b.globals.tiltDbPerOctave * b.globals.amount
	       && a.globals.tiltPivotHz == b.globals.tiltPivotHz;
}

bool EqGraph::anyMidSide(const View& v) {
	for (size_t b = 0; b < v.designs.size(); ++b) {
		if (v.channel[b] != eq::Channel::Stereo && !v.designs[b].bypass) {
			return true;
		}
	}
	return false;
}

visage::Point EqGraph::handleAt(const View& v, int band) const {
	const eq_plot::Geometry g = geometry();
	const eq::EffectiveBand& e = v.bands[static_cast<size_t>(band)];
	if (!e.on) {
		const visage::Bounds rail = railBounds();
		const double hz = std::clamp(v.unclampedHz[static_cast<size_t>(band)], eq_plot::kLowHz, eq_plot::kHighHz);
		return { g.xForHz(hz), rail.y() + rail.height() * 0.5f };
	}
	const double db = isGainType(e.type) ? std::clamp(e.gainDb, -static_cast<double>(g.rangeDb),
	                                                  static_cast<double>(g.rangeDb))
	                                     : 0.0;
	const double hz = std::clamp(e.freqHz, eq_plot::kLowHz, eq_plot::kHighHz);
	return { g.xForHz(hz), g.yForDb(db) };
}

visage::Point EqGraph::pivotAt(const View& v) const {
	const eq_plot::Geometry g = geometry();
	return { g.xForHz(std::clamp(v.globals.tiltPivotHz, eq_plot::kLowHz, eq_plot::kHighHz)), g.yForDb(0.0) };
}

int EqGraph::bandAt(visage::Point p, const View& v) const {
	int best = -1;
	float bestD = kGrabRadius;
	// The SELECTED band first, so two stacked handles give the hand the one it is already working on.
	const int first = selected();
	for (int i = 0; i < eq::kBandCount; ++i) {
		const int b = (first + i) % eq::kBandCount;
		if (!v.bands[static_cast<size_t>(b)].on) {
			continue;
		}
		const visage::Point h = handleAt(v, b);
		const float d = std::hypot(h.x - p.x, h.y - p.y);
		if (d < bestD) {
			bestD = d;
			best = b;
		}
	}
	return best;
}

std::string EqGraph::crosshairText(visage::Point p) const {
	const View v = view();
	const double hz = geometry().hzForX(p.x);
	const double db = pathDb(v, v.viewChannel, hz);
	char gain[32] = {};
	std::snprintf(gain, sizeof(gain), "   %s %+.1f dB",
	              v.viewChannel == eq::Channel::Stereo ? "EQ" : eq::kChannelNames[static_cast<size_t>(v.viewChannel)], db);
	std::string text = fx::eq::frequencyReadout(hz) + gain;
	// THE ANALYSER AT THE POINTER (§6.5), as drawn — each tap only if it is on.
	if (analyser_.framesTaken > 0 && (preOn() || postOn())) {
		char levels[48] = {};
		if (preOn() && postOn()) {
			std::snprintf(levels, sizeof(levels), "   pre %.0f  post %.0f dB", static_cast<double>(analyserDbAt(analyser_.pre, hz)),
			              static_cast<double>(analyserDbAt(analyser_.post, hz)));
		}
		else {
			std::snprintf(levels, sizeof(levels), "   %s %.0f dB", preOn() ? "pre" : "post",
			              static_cast<double>(analyserDbAt(preOn() ? analyser_.pre : analyser_.post, hz)));
		}
		text += levels;
	}
	return text;
}

int EqGraph::extraChoice(eq::Extra e, int count) const {
	return std::clamp(static_cast<int>(std::lround(field(eq::extraIndex(e)))), 0, count - 1);
}

void EqGraph::setExtra(eq::Extra e, float value) {
	host_.setField(device_, eq::extraIndex(e), value);
	redraw();
}

bool EqGraph::preOn() const {
	return field(eq::extraIndex(eq::Extra::Pre)) < 0.5f;
}

bool EqGraph::postOn() const {
	return field(eq::extraIndex(eq::Extra::Post)) < 0.5f;
}

float EqGraph::analyserTiltDb() const {
	return eq::kAnalyserTiltDb[static_cast<size_t>(
	    extraChoice(eq::Extra::AnalyserTilt, static_cast<int>(std::size(eq::kAnalyserTiltDb))))];
}

float EqGraph::analyserDbAt(const std::array<float, eq::kAnalyserBins>& bins, double hz) const {
	// Bin k's centre is at 20 * 1000^((k + 0.5) / 96): invert, and interpolate linearly in log frequency.
	const double pos = std::log10(std::clamp(hz, eq::kAnalyserLowHz, eq::kAnalyserHighHz) / eq::kAnalyserLowHz)
	                       / std::log10(eq::kAnalyserHighHz / eq::kAnalyserLowHz) * static_cast<double>(eq::kAnalyserBins)
	                   - 0.5;
	const int i = std::clamp(static_cast<int>(std::floor(pos)), 0, static_cast<int>(eq::kAnalyserBins) - 2);
	const float t = static_cast<float>(std::clamp(pos - i, 0.0, 1.0));
	const float db = bins[static_cast<size_t>(i)] + t * (bins[static_cast<size_t>(i) + 1] - bins[static_cast<size_t>(i)]);
	// SLOPE COMPENSATION, pivoting at 1 kHz (§4.3).
	return db + analyserTiltDb() * static_cast<float>(std::log2(hz / 1000.0));
}

bool EqGraph::advanceAnalyser() {
	bool moved = false;
	// A NEW VIEW STARTS FROM NOTHING: what was shown, and its held peaks, were another signal's.
	const int view = static_cast<int>(viewChannel());
	if (view != shownView_) {
		shownView_ = view;
		for (auto* a : { &analyser_.pre, &analyser_.post, &analyser_.peak, &rawPre_, &rawPost_ }) {
			a->fill(eq::kAnalyserFloorDb);
		}
		peakAge_.fill(0.0f);
		moved = true;
	}
	// A WHOLE FRAME OR NONE: the counter is written first and last, and a reader that sees the two differ has
	// caught the device mid-write (the readings store has no seqlock).
	if (host_.readingCountAt(device_) >= eq::kReadingCount) {
		const float a = host_.readingAt(device_, static_cast<size_t>(eq::Reading::FrameA));
		if (a != lastFrame_ && a > 0.0f) {
			std::array<float, eq::kAnalyserBins> pre {}, post {};
			for (size_t k = 0; k < eq::kAnalyserBins; ++k) {
				pre[k] = host_.readingAt(device_, static_cast<size_t>(eq::Reading::PreFirst) + k);
				post[k] = host_.readingAt(device_, static_cast<size_t>(eq::Reading::PostFirst) + k);
			}
			const float frameView = host_.readingAt(device_, static_cast<size_t>(eq::Reading::View));
			const float b = host_.readingAt(device_, static_cast<size_t>(eq::Reading::FrameB));
			if (a == b && static_cast<int>(frameView) != shownView_) {
				// COMPUTED IN ANOTHER VIEW — the device has not caught up with a change yet (one hop at most).
				lastFrame_ = a;
				++analyser_.framesOtherView;
			}
			else if (a == b) {
				rawPre_ = pre;
				rawPost_ = post;
				lastFrame_ = a;
				++analyser_.framesTaken;
				moved = true;
			}
			else {
				++analyser_.framesDropped;
			}
		}
	}
	// THE BALLISTICS: attack instant, release a one-pole in dB at the chosen speed.
	const float seconds = eq::kAnalyserSpeedSeconds[static_cast<size_t>(
	    extraChoice(eq::Extra::AnalyserSpeed, static_cast<int>(std::size(eq::kAnalyserSpeedSeconds))))];
	const float release = 1.0f - std::exp(-kTickSeconds / seconds);
	const float hold = eq::kAnalyserPeakSeconds[static_cast<size_t>(
	    extraChoice(eq::Extra::AnalyserPeak, static_cast<int>(std::size(eq::kAnalyserPeakSeconds))))];
	for (size_t k = 0; k < eq::kAnalyserBins; ++k) {
		for (const auto& [shown, raw] : { std::pair { &analyser_.pre[k], rawPre_[k] }, std::pair { &analyser_.post[k], rawPost_[k] } }) {
			const float next = raw >= *shown ? raw : *shown + (raw - *shown) * release;
			if (std::fabs(next - *shown) > 0.01f) {
				moved = true;
			}
			*shown = next;
		}
		// PEAK HOLD, on the post tap: kept until it ages out (2 s), or until cleared (Hold).
		peakAge_[k] += kTickSeconds;
		if (hold == 0.0f) {
			analyser_.peak[k] = eq::kAnalyserFloorDb;
		}
		else if (analyser_.post[k] >= analyser_.peak[k] || (hold > 0.0f && peakAge_[k] > hold)) {
			if (analyser_.peak[k] != analyser_.post[k]) {
				moved = true;
			}
			analyser_.peak[k] = analyser_.post[k];
			peakAge_[k] = 0.0f;
		}
	}
	return moved;
}

bool EqGraph::update() {
	if (!isVisible()) {
		return false;
	}
	// THE MEMBER BUFFER, cleared and refilled: a tick at rest allocates nothing (its capacity is kept, and the swap below
	// hands the old vector's to the next tick).
	std::vector<float>& now = scratchFields_;
	now.clear();
	for (size_t i = 0; i < eq::kParamCount; ++i) {
		now.push_back(field(i)); // the base: the curve with the handles
		now.push_back(modulated(i));
	}
	for (const eq::Extra e : { eq::Extra::Selected, eq::Extra::DbRange, eq::Extra::Pre, eq::Extra::Post, eq::Extra::AnalyserChannel,
	                           eq::Extra::AnalyserTilt, eq::Extra::AnalyserSpeed, eq::Extra::AnalyserPeak }) {
		now.push_back(field(eq::extraIndex(e)));
	}
	for (int b = 0; b < eq::kBandCount; ++b) {
		now.push_back(field(eq::extraIndex(eq::channelField(b))));
	}
	now.push_back(static_cast<float>(keyOctaves()));
	const bool analyserMoved = (preOn() || postOn()) && advanceAnalyser();
	if (now == drawnFields_ && !analyserMoved) {
		return false;
	}
	drawnFields_.swap(now);
	redraw();
	return true;
}

void EqGraph::drawGrid(visage::Canvas& canvas, const eq_plot::Geometry& g) {
	const visage::Font& face = font(kLabelSize);
	// Frequency lines, by tier.
	for (const eq_plot::FreqLine& line : eq_plot::frequencyLines(g)) {
		const float x = g.xForHz(line.hz);
		const float a = line.tier == eq_plot::Tier::Decade ? 0.55f : line.tier == eq_plot::Tier::TwoFive ? 0.35f : 0.20f;
		canvas.setColor(withAlpha(kLine, a));
		canvas.segment(x, g.y, x, g.y + g.height, 1.0f, true);
	}
	// Octave C lines, once an octave is wide enough to carry one (the musical grid).
	if (eq_plot::drawsNoteLines(g)) {
		canvas.setColor(withAlpha(kAccent, 0.12f));
		for (const double hz : eq_plot::noteCs()) {
			const float x = g.xForHz(hz);
			canvas.segment(x, g.y, x, g.y + g.height, 1.0f, true);
			const auto n = fx::eq::noteOf(hz);
			canvas.setColor(withAlpha(kAccent, 0.45f));
			canvas.text(fx::eq::noteName(n.midi), face, visage::Font::kLeft, x + 2.0f, g.y + 1.0f, 30.0f, kLabelSize + 2.0f);
			canvas.setColor(withAlpha(kAccent, 0.12f));
		}
	}
	// dB lines.
	const float step = eq_plot::dbStep(g);
	for (float db = -g.rangeDb; db <= g.rangeDb + 1e-3f; db += step) {
		const float y = g.yForDb(db);
		canvas.setColor(withAlpha(kLine, db == 0.0f ? 0.9f : 0.30f));
		canvas.segment(g.x, y, g.x + g.width, y, db == 0.0f ? 1.5f : 1.0f, true);
		if (!compact_ && std::fabs(db) < g.rangeDb - 1e-3f) {
			char label[16] = {};
			std::snprintf(label, sizeof(label), "%+.0f", static_cast<double>(db));
			canvas.setColor(withAlpha(kMuted, 0.8f));
			canvas.text(label, face, visage::Font::kLeft, g.x + 3.0f, y - kLabelSize - 1.0f, 30.0f, kLabelSize + 1.0f);
		}
	}
	// The dB range, top left: on the panel it is also the zoom's menu.
	char range[16] = {};
	std::snprintf(range, sizeof(range), "\xC2\xB1%.0f dB", static_cast<double>(g.rangeDb));
	canvas.setColor(withAlpha(kMuted, compact_ ? 0.6f : 0.9f));
	canvas.text(range, face, visage::Font::kLeft, g.x + 3.0f, g.y + 2.0f, 40.0f, kLabelSize + 2.0f);
	// Frequency labels along the bottom, only where they fit.
	const float charWidth = std::max(1.0f, trackedWidth(face, "8", 0.0f));
	canvas.setColor(withAlpha(kMuted, 0.85f));
	for (const eq_plot::Label& l : eq_plot::frequencyLabels(g, charWidth)) {
		canvas.text(l.text, face, visage::Font::kLeft, l.left, g.y + g.height - kLabelSize - 2.0f,
		            l.right - l.left + 2.0f, kLabelSize + 1.0f);
	}
}

void EqGraph::drawCurve(visage::Canvas& canvas, const View& v, const eq_plot::Geometry& g, bool live) {
	const int points = std::clamp(static_cast<int>(g.width * kPointsPerPixel), kMinPoints, kMaxPoints);
	const auto trace = [&](eq::Channel path, float width) {
		float prevX = 0.0f, prevY = 0.0f;
		bool have = false;
		for (int i = 0; i < points; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(points - 1);
			const float x = g.x + t * g.width;
			const double db = pathDb(v, path, g.hzForX(x));
			if (!std::isfinite(db)) {
				continue;
			}
			const float y = std::clamp(g.yForDb(db), g.y, g.y + g.height);
			if (have) {
				canvas.segment(prevX, prevY, x, y, width, true);
			}
			prevX = x;
			prevY = y;
			have = true;
		}
		return visage::Point { prevX, prevY };
	};
	// THE HEARD CURVE (MP): the viewed path only, thinner and in the neutral ink, over the base curve it departs from.
	if (live) {
		canvas.setColor(withAlpha(kInk, 0.55f));
		trace(v.viewChannel, 1.25f);
		return;
	}
	// THE STEREO VIEW WITH M/S BANDS ACTING: the Mid and Side paths too, thin and faint and named at their
	// right-hand ends, so a Mid or Side band is never invisible (assumption A21).
	if (v.viewChannel == eq::Channel::Stereo && anyMidSide(v)) {
		const visage::Font& face = font(kLabelSize);
		for (const eq::Channel path : { eq::Channel::Mid, eq::Channel::Side }) {
			canvas.setColor(withAlpha(kAccent, 0.40f));
			const visage::Point end = trace(path, 1.0f);
			canvas.setColor(withAlpha(kAccent, 0.70f));
			canvas.text(path == eq::Channel::Mid ? "M" : "S", face, visage::Font::kRight, end.x - 12.0f,
			            end.y - kLabelSize - 1.0f, 10.0f, kLabelSize + 1.0f);
		}
	}
	canvas.setColor(withAlpha(kAccent, 0.95f));
	trace(v.viewChannel, 2.0f);
}

void EqGraph::drawHandles(visage::Canvas& canvas, const View& v) {
	const int sel = selected();
	const visage::Font& face = font(kLabelSize);
	// The rail's guide line.
	const visage::Bounds rail = railBounds();
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.segment(rail.x(), rail.y() + rail.height() * 0.5f, rail.right(), rail.y() + rail.height() * 0.5f, 1.0f, true);
	for (int b = 0; b < eq::kBandCount; ++b) {
		const visage::Point h = handleAt(v, b);
		const bool on = v.bands[static_cast<size_t>(b)].on;
		const float r = on ? kHandleRadius : kHandleRadius - 2.0f;
		const uint32_t hue = bandHue(b);
		// OUTSIDE THE VIEW (step 6): a Mid band in the Side view, any M/S band in the Stereo view — dimmed, still
		// live to the hand.
		const float a = inPath(v.channel[static_cast<size_t>(b)], v.viewChannel) ? 1.0f : kOutOfViewAlpha;
		if (b == sel) {
			canvas.setColor(withAlpha(hue, a));
			canvas.circle(h.x - r, h.y - r, 2.0f * r);
			canvas.setColor(withAlpha(kInk, a));
			canvas.ring(h.x - r - 2.0f, h.y - r - 2.0f, 2.0f * (r + 2.0f), 1.5f);
		}
		else {
			canvas.setColor(withAlpha(kSurface, 0.85f * a));
			canvas.circle(h.x - r, h.y - r, 2.0f * r);
			canvas.setColor(withAlpha(hue, (on ? 1.0f : 0.6f) * a));
			canvas.ring(h.x - r, h.y - r, 2.0f * r, 1.5f);
		}
		if (on && !compact_) {
			// THE NUMBER STAYS INSIDE THE PLOT: up and to the right of its handle, or to the left and below
			// where that would leave the plot (a band at 20 kHz, a band at the top of the range).
			const visage::Bounds plot = plotBounds();
			constexpr float kLabelW = 16.0f;
			const bool flip = h.x + r + 2.0f + kLabelW > plot.right();
			const float lx = flip ? h.x - r - 2.0f - kLabelW : h.x + r + 2.0f;
			float ly = h.y - r - kLabelSize;
			if (ly < plot.y()) {
				ly = h.y + r;
			}
			canvas.setColor(withAlpha(hue, 0.9f * a));
			// A Mid or Side band says so beside its number: "3M", "5S".
			const eq::Channel ch = v.channel[static_cast<size_t>(b)];
			const std::string label = std::to_string(b + 1) + (ch == eq::Channel::Mid ? "M" : ch == eq::Channel::Side ? "S" : "");
			canvas.text(label, face, flip ? visage::Font::kRight : visage::Font::kLeft, lx, ly, kLabelW,
			            kLabelSize + 1.0f);
		}
	}
	// THE PIVOT: a small diamond on the 0 dB line, only while there is a tilt to turn.
	if (field(eq::paramIndex(eq::Param::Tilt)) != 0.0f) { // the STORED tilt: the base view has none
		const visage::Point p = pivotAt(v);
		constexpr float d = 4.5f;
		canvas.setColor(withAlpha(kInk, 0.85f));
		canvas.segment(p.x - d, p.y, p.x, p.y - d, 1.5f, true);
		canvas.segment(p.x, p.y - d, p.x + d, p.y, 1.5f, true);
		canvas.segment(p.x + d, p.y, p.x, p.y + d, 1.5f, true);
		canvas.segment(p.x, p.y + d, p.x - d, p.y, 1.5f, true);
	}
}

void EqGraph::drawCrosshair(visage::Canvas& canvas, const eq_plot::Geometry& g) {
	const visage::Font& face = font(kLabelSize);
	std::string text;
	if (hovering_ && !drag_.active && plotBounds().contains(hover_)) {
		canvas.setColor(withAlpha(kInk, 0.25f));
		canvas.segment(hover_.x, g.y, hover_.x, g.y + g.height, 1.0f, true);
		canvas.segment(g.x, hover_.y, g.x + g.width, hover_.y, 1.0f, true);
		text = crosshairText(hover_);
	}
	else {
		// THE MUSICAL READOUT of the selected band (§6.4) when nothing is hovered.
		const View v = view();
		const int sel = selected();
		const eq::EffectiveBand& e = v.bands[static_cast<size_t>(sel)];
		text = "Band " + std::to_string(sel + 1) + "  " + fx::eq::frequencyReadout(e.freqHz);
	}
	const float w = trackedWidth(face, text, 0.0f) + 8.0f;
	const float x = g.x + g.width - w - 2.0f;
	canvas.setColor(withAlpha(kSurface, 0.85f));
	canvas.roundedRectangle(x, g.y + 2.0f, w, kLabelSize + 4.0f, 3.0f);
	canvas.setColor(kInk);
	canvas.text(text, face, visage::Font::kLeft, x + 4.0f, g.y + 3.0f, w, kLabelSize + 2.0f);
}

void EqGraph::draw(visage::Canvas& canvas) {
	const eq_plot::Geometry g = geometry();
	canvas.setColor(withAlpha(kSurface, 0.6f));
	canvas.roundedRectangle(g.x, g.y, g.width, g.height, 3.0f);
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.rectangleBorder(g.x, g.y, g.width, g.height, 1.0f);
	drawGrid(canvas, g);
	drawAnalyser(canvas, g);
	// THE BASE CURVE CARRIES THE HANDLES AND STAYS STILL; THE HEARD ONE MOVES OVER IT, quieter and with none (MP).
	const View base = baseView();
	const View live = view();
	drawCurve(canvas, base, g, false);
	if (!sameResponse(base, live)) {
		drawCurve(canvas, live, g, true);
	}
	drawHandles(canvas, base);
	drawCrosshair(canvas, g);
}

void EqGraph::beginDrag(int band, visage::Point p, bool fine) {
	const View v = baseView();
	const eq::EffectiveBand& e = v.bands[static_cast<size_t>(band)];
	drag_ = {};
	drag_.active = true;
	drag_.band = band;
	drag_.fine = fine;
	drag_.anchor = p;
	drag_.yIsGain = isGainType(e.type);
	drag_.anchorFreqLog2 = field(eq::bandIndex(band, eq::BandField::Freq));
	drag_.anchorY = field(eq::bandIndex(band, drag_.yIsGain ? eq::BandField::Gain : eq::BandField::Q));
	control_(eq::bandIndex(band, eq::BandField::Freq)).beginGesture();
	control_(eq::bandIndex(band, drag_.yIsGain ? eq::BandField::Gain : eq::BandField::Q)).beginGesture();
}

void EqGraph::dragTo(visage::Point p, bool fine) {
	if (fine != drag_.fine) {
		// RE-ANCHOR RATHER THAN RESCALE WHAT CAME BEFORE (knob.cpp:243): the modifier changes the rate
		// from here on, not the distance already travelled.
		drag_.fine = fine;
		drag_.anchor = p;
		drag_.anchorFreqLog2 = field(drag_.pivot ? eq::paramIndex(eq::Param::TiltPivot)
		                                         : eq::bandIndex(drag_.band, eq::BandField::Freq));
		if (!drag_.pivot) {
			drag_.anchorY = field(eq::bandIndex(drag_.band, drag_.yIsGain ? eq::BandField::Gain : eq::BandField::Q));
		}
		return;
	}
	const eq_plot::Geometry g = geometry();
	const double factor = fine ? kFineFactor : 1.0;
	const double dxOct = static_cast<double>(p.x - drag_.anchor.x) / static_cast<double>(g.pixelsPerOctave()) * factor;
	if (drag_.pivot) {
		DeviceControl& c = control_(eq::paramIndex(eq::Param::TiltPivot));
		c.setValue(std::clamp(drag_.anchorFreqLog2 + dxOct, c.desc().min, c.desc().max));
		redraw();
		return;
	}
	DeviceControl& freq = control_(eq::bandIndex(drag_.band, eq::BandField::Freq));
	freq.setValue(std::clamp(drag_.anchorFreqLog2 + dxOct, freq.desc().min, freq.desc().max));
	{
		const double dy = static_cast<double>(drag_.anchor.y - p.y) * factor;
		if (drag_.yIsGain) {
			// ONE TO ONE UNDER THE POINTER (MP): the handle sits at the STORED gain, before Amount, so a pixel is a pixel of
			// dB. (This divided by Amount while the handle sat on the heard curve, and wrote nothing below |Amount| 5%,
			// where the division could not be read back; with the handle on the base there is no division to guard.)
			DeviceControl& gain = control_(eq::bandIndex(drag_.band, eq::BandField::Gain));
			const double dDb = dy * (2.0 * static_cast<double>(g.rangeDb) / static_cast<double>(g.height));
			gain.setValue(std::clamp(drag_.anchorY + dDb, gain.desc().min, gain.desc().max));
		}
		else {
			DeviceControl& q = control_(eq::bandIndex(drag_.band, eq::BandField::Q));
			q.setValue(std::clamp(drag_.anchorY + dy / static_cast<double>(kPxPerQOctave), q.desc().min, q.desc().max));
		}
	}
	redraw();
}

void EqGraph::endDrag() {
	if (!drag_.active) {
		return;
	}
	if (drag_.pivot) {
		control_(eq::paramIndex(eq::Param::TiltPivot)).endGesture();
	}
	else {
		control_(eq::bandIndex(drag_.band, eq::BandField::Freq)).endGesture();
		control_(eq::bandIndex(drag_.band, drag_.yIsGain ? eq::BandField::Gain : eq::BandField::Q)).endGesture();
	}
	drag_ = {};
	redraw();
}

void EqGraph::showTypeMenu(int band, visage::Point at) {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(eq::kTypeNames); ++i) {
		menu.addOption(static_cast<int>(i), eq::kTypeNames[i]);
	}
	menu.onSelection() = [this, band](int id) {
		if (id < 0 || id >= static_cast<int>(eq::BandType::kCount)) {
			return;
		}
		DeviceControl& type = control_(eq::bandIndex(band, eq::BandField::Type));
		type.beginGesture();
		type.setValue(static_cast<double>(id));
		type.endGesture();
		redraw();
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

void EqGraph::showRangeMenu(visage::Point at) {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(eq::kDbRanges); ++i) {
		char label[16] = {};
		std::snprintf(label, sizeof(label), "\xC2\xB1%.0f dB", static_cast<double>(eq::kDbRanges[i]));
		menu.addOption(static_cast<int>(i), label);
	}
	menu.onSelection() = [this](int id) {
		if (id < 0 || id >= static_cast<int>(std::size(eq::kDbRanges))) {
			return;
		}
		host_.setField(device_, eq::extraIndex(eq::Extra::DbRange), static_cast<float>(id));
		redraw();
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

visage::Bounds EqGraph::analyserChipBounds() const {
	const visage::Bounds p = plotBounds();
	return { p.x() + 44.0f, p.y() + 1.0f, 64.0f, kLabelSize + 4.0f };
}

visage::Bounds EqGraph::viewChipBounds() const {
	const visage::Bounds a = analyserChipBounds();
	return { a.right() + 4.0f, a.y(), 52.0f, a.height() };
}

void EqGraph::showViewMenu(visage::Point at) {
	// ALL THREE ON THE FACE OF THE MENU, the current one checked (the multivalue-toggle rule).
	visage::PopupMenu menu;
	const eq::Channel now = viewChannel();
	for (size_t i = 0; i < std::size(eq::kChannelNames); ++i) {
		menu.addOption(static_cast<int>(i), eq::kChannelNames[i]).select(static_cast<int>(i) == static_cast<int>(now));
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0 && id < static_cast<int>(eq::Channel::kCount)) {
			setExtra(eq::Extra::AnalyserChannel, static_cast<float>(id));
		}
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

void EqGraph::drawAnalyser(visage::Canvas& canvas, const eq_plot::Geometry& g) {
	const visage::Font& face = font(kLabelSize);
	if (!compact_) {
		// THE CHIP that opens the analyser menu, and the analyser's own axis on the right (0 to -90 dBFS).
		const visage::Bounds chip = analyserChipBounds();
		canvas.setColor(withAlpha(kMuted, 0.9f));
		canvas.text("Analyser \xE2\x96\xBE", face, visage::Font::kLeft, chip.x(), chip.y() + 1.0f, chip.width(),
		            kLabelSize + 2.0f);
		// THE VIEW CHIP beside it: the path the curve and the analyser show (step 6).
		const visage::Bounds viewChip = viewChipBounds();
		const std::string viewText = std::string(eq::kChannelNames[static_cast<size_t>(viewChannel())]) + " \xE2\x96\xBE";
		canvas.setColor(withAlpha(viewChannel() == eq::Channel::Stereo ? kMuted : kAccent, 0.9f));
		canvas.text(viewText, face, visage::Font::kLeft, viewChip.x(), viewChip.y() + 1.0f, viewChip.width(),
		            kLabelSize + 2.0f);
		// LINEAR PHASE SAYS WHAT IT COSTS (step 7): the latency the host is compensating, in ms.
		if (field(eq::paramIndex(eq::Param::Phase)) >= 0.5f) {
			const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
			char latency[32] = {};
			std::snprintf(latency, sizeof(latency), "Linear  %.0f ms",
			              1000.0 * static_cast<double>(eq::linearLatency(rate)) / rate);
			canvas.setColor(withAlpha(kAccent, 0.8f));
			canvas.text(latency, face, visage::Font::kLeft, viewChip.right() + 6.0f, viewChip.y() + 1.0f, 80.0f,
			            kLabelSize + 2.0f);
		}
		if (preOn() || postOn()) {
			canvas.setColor(withAlpha(kMuted, 0.55f));
			for (const float db : { -30.0f, -60.0f }) {
				const float y = g.y + g.height * (-db / kAnalyserRangeDb);
				char label[8] = {};
				std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(db));
				canvas.text(label, face, visage::Font::kRight, g.x + g.width - 30.0f, y - kLabelSize - 1.0f, 27.0f,
				            kLabelSize + 1.0f);
			}
		}
	}
	else {
		// THE CARD HAS NO CHIPS, so a view other than Stereo, Linear phase and Key Track, are named where they would be.
		std::string named = viewChannel() != eq::Channel::Stereo ? eq::kChannelNames[static_cast<size_t>(viewChannel())] : "";
		if (field(eq::paramIndex(eq::Param::Phase)) >= 0.5f) {
			named += named.empty() ? "Linear" : " \xC2\xB7 Linear";
		}
		if (field(eq::paramIndex(eq::Param::KeyTrack)) >= 0.5f) {
			named += named.empty() ? "Key Track" : " \xC2\xB7 Key Track";
		}
		if (!named.empty()) {
			const visage::Bounds chip = analyserChipBounds();
			canvas.setColor(withAlpha(kAccent, 0.8f));
			canvas.text(named, face, visage::Font::kLeft, chip.x(), chip.y() + 1.0f, chip.width() + 100.0f,
			            kLabelSize + 2.0f);
		}
	}
	if (analyser_.framesTaken == 0 || (!preOn() && !postOn())) {
		return;
	}
	const auto yAt = [&](float db) {
		return std::clamp(g.y + g.height * (-db / kAnalyserRangeDb), g.y, g.y + g.height);
	};
	const int columns = std::max(2, static_cast<int>(g.width));
	// PRE: a filled area only, in the grid's neutral ink — the room the EQ was given (§4.2).
	if (preOn()) {
		canvas.setColor(withAlpha(kInk, 0.10f));
		for (int c = 0; c < columns; c += 2) {
			const float x = g.x + static_cast<float>(c);
			const float y = yAt(analyserDbAt(analyser_.pre, g.hzForX(x)));
			canvas.segment(x, y, x, g.y + g.height, 2.0f, false);
		}
	}
	// POST: a line in the accent (with a faint fill of its own only when PRE is off, so each tap keeps its
	// own mark whatever else is on), and its held peaks dotted above it.
	if (postOn()) {
		if (!preOn()) {
			canvas.setColor(withAlpha(kAccent, 0.06f));
			for (int c = 0; c < columns; c += 2) {
				const float x = g.x + static_cast<float>(c);
				canvas.segment(x, yAt(analyserDbAt(analyser_.post, g.hzForX(x))), x, g.y + g.height, 2.0f, false);
			}
		}
		canvas.setColor(withAlpha(kAccent, 0.55f));
		float px = g.x, py = yAt(analyserDbAt(analyser_.post, g.hzForX(g.x)));
		for (int c = 2; c <= columns; c += 2) {
			const float x = g.x + static_cast<float>(std::min(c, columns - 1));
			const float y = yAt(analyserDbAt(analyser_.post, g.hzForX(x)));
			canvas.segment(px, py, x, y, 1.5f, true);
			px = x;
			py = y;
		}
		if (analyser_.peak[0] > eq::kAnalyserFloorDb || analyser_.peak[eq::kAnalyserBins / 2] > eq::kAnalyserFloorDb) {
			canvas.setColor(withAlpha(kAccent, 0.35f));
			for (int c = 0; c + 3 < columns; c += 8) {
				const float x0 = g.x + static_cast<float>(c), x1 = x0 + 3.0f;
				canvas.segment(x0, yAt(analyserDbAt(analyser_.peak, g.hzForX(x0))), x1,
				               yAt(analyserDbAt(analyser_.peak, g.hzForX(x1))), 1.0f, true);
			}
		}
	}
	// THE LEGEND, only when both taps are drawn and there is room for it.
	if (preOn() && postOn() && !compact_) {
		const float y = g.y + g.height - 2.0f * kLabelSize - 6.0f;
		canvas.setColor(withAlpha(kInk, 0.45f));
		canvas.text("pre", face, visage::Font::kRight, g.x + g.width - 60.0f, y, 26.0f, kLabelSize + 1.0f);
		canvas.setColor(withAlpha(kAccent, 0.8f));
		canvas.text("post", face, visage::Font::kRight, g.x + g.width - 32.0f, y, 28.0f, kLabelSize + 1.0f);
	}
}

void EqGraph::showAnalyserMenu(visage::Point at) {
	// EVERY CHOICE VISIBLE BEFORE IT IS APPLIED (the multivalue-toggle rule): the two taps as check items,
	// the rest as radio sub-menus.
	enum : int { kPre = 1, kPost = 2, kTilt = 10, kSpeed = 20, kPeak = 30 };
	visage::PopupMenu menu;
	menu.addOption(kPre, "Pre").select(preOn());
	menu.addOption(kPost, "Post").select(postOn());
	const auto radio = [&](const char* name, int base, const auto& names, eq::Extra e) {
		std::vector<visage::PopupMenu> options;
		const int now = extraChoice(e, static_cast<int>(std::size(names)));
		for (size_t i = 0; i < std::size(names); ++i) {
			options.emplace_back(names[i], base + static_cast<int>(i));
			options.back().select(static_cast<int>(i) == now);
		}
		menu.addOption(-1, name) = visage::PopupMenu(name, -1, std::move(options));
	};
	radio("Tilt", kTilt, eq::kAnalyserTiltNames, eq::Extra::AnalyserTilt);
	radio("Speed", kSpeed, eq::kAnalyserSpeedNames, eq::Extra::AnalyserSpeed);
	radio("Peak hold", kPeak, eq::kAnalyserPeakNames, eq::Extra::AnalyserPeak);
	menu.onSelection() = [this](int id) {
		if (id == kPre) {
			setExtra(eq::Extra::Pre, preOn() ? 1.0f : 0.0f);
		}
		else if (id == kPost) {
			setExtra(eq::Extra::Post, postOn() ? 1.0f : 0.0f);
		}
		else if (id >= kTilt && id < kTilt + static_cast<int>(std::size(eq::kAnalyserTiltNames))) {
			setExtra(eq::Extra::AnalyserTilt, static_cast<float>(id - kTilt));
		}
		else if (id >= kSpeed && id < kSpeed + static_cast<int>(std::size(eq::kAnalyserSpeedNames))) {
			setExtra(eq::Extra::AnalyserSpeed, static_cast<float>(id - kSpeed));
		}
		else if (id >= kPeak && id < kPeak + static_cast<int>(std::size(eq::kAnalyserPeakNames))) {
			// CHOOSING A PEAK MODE — even the one already chosen — clears the held peaks (A16).
			analyser_.peak.fill(eq::kAnalyserFloorDb);
			peakAge_.fill(0.0f);
			setExtra(eq::Extra::AnalyserPeak, static_cast<float>(id - kPeak));
		}
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

void EqGraph::offeredControls(std::vector<Offered>& out) const {
	// EVERY BAND'S HANDLE: dragged, it writes Freq across and Gain or Q up; a press toggles On; its menu picks Type. And
	// the tilt's pivot. Side roads, all of them — the bands' boxes are the faces.
	for (int b = 0; b < eq::kBandCount; ++b) {
		for (const eq::BandField f : { eq::BandField::Freq, eq::BandField::Gain, eq::BandField::Q, eq::BandField::On,
		                               eq::BandField::Type }) {
			out.push_back({ &control_(eq::bandIndex(b, f)), Road::Gesture });
		}
	}
	out.push_back({ &control_(eq::paramIndex(eq::Param::TiltPivot)), Road::Gesture });
}

} // namespace feathers::ui
