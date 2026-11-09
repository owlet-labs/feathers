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

#include "plugin/ui/phaser_plot.h"

#include "plugin/ui/celestial.h" // reducedMotionRequested — the barberpole's stripes hold still under it
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace ph = fx::phaser;

namespace {

constexpr float kInsetCard = 4.0f;
constexpr float kInsetLarge = 8.0f;
constexpr float kFootHeight = 7.0f;
/// THE STRIPES' PERIOD AND WIDTH, pixels: one ramp cycle moves them one period.
constexpr float kStripePeriod = 12.0f;
constexpr float kStripeWidth = 4.0f;
/// The fewest points a curve is computed at, whatever the width — a frame laid out at zero still draws a line.
constexpr size_t kMinPoints = 16;
/// THE SUB-SAMPLES ACROSS ONE PIXEL for its span bar (`PhaserPlot::loL_`).
constexpr int kSubSamples = 4;
/// A SPAN UNDER THIS MANY dB IS THE LINE ITSELF and draws no bar.
constexpr float kSpanBarDb = 3.0f;

double log10Of(double v) { return std::log10(std::max(v, 1e-9)); }

void formatHz(char* out, size_t size, double hz) {
	if (hz >= 1000.0) {
		std::snprintf(out, size, "%.1fk", hz / 1000.0);
	}
	else {
		std::snprintf(out, size, "%.0f", hz);
	}
}

} // namespace

ph::Settings phaserSettingsAt(const backplane::DeviceHost& host, DeviceInstanceId id, bool modulated) {
	ph::Row row {};
	const size_t present = host.fieldsPresentAt(id);
	const size_t take = std::min(present, row.size());
	for (size_t i = 0; i < take; ++i) {
		row[i] = modulated ? host.modulatedFieldAt(id, i) : host.fieldAt(id, i);
	}
	// THE OPS TABLE'S `setFields`, read for read: absent params at their defaults, then the folded travel past them.
	ph::Settings s = ph::stateOf(row, take).settings;
	for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
		const size_t at = ph::kTravelFirst + i;
		const float v = at < present ? host.fieldAt(id, at) : 0.0f;
		s.travel[i] = std::isfinite(v) ? v : 0.0f;
	}
	return s;
}

PhaserPlot::PhaserPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large)
    : host_(host), device_(id), large_(large) {
	controls_.resize(ph::kParamCount);

	// A 2-D HANDLE (a drag on the response plot acts on both axes, in every mode): x is the centre control — Center, or Delay in the Flanger, in every state (while a note is tracked the
	// knob is the offset from it) — and y is Feedback. BOTH RELATIVE to the press (`valueForDrag`, `feedbackForDrag`),
	// so a press writes nothing; AT MOST ONE GESTURE A CONTROL, opened on that axis's first real change (y past a dead
	// zone) and ended at the release; Shift is the house fine factor and re-anchors, never rescales the travel
	// already made (knob.cpp's rule). With a source ARMED the drag is the centre control's route amount, the gesture
	// every plot and knob here takes (`DisperserPlot`, `Knob`) — vertical, so y is that amount and not Feedback.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		drag_ = {};
		const size_t index = draggedControl();
		DeviceControl& ctrl = control(index);
		drag_.active = true;
		drag_.control = index;
		drag_.startX = e.position.x;
		drag_.startY = e.position.y;
		drag_.startValue = ctrl.value();
		drag_.fine = e.isShiftDown();
		if (!e.isAltDown() && ctrl.display().routable) {
			drag_.route = true;
			const float before = ctrl.display().amount;
			drag_.anchor = before != 0.0f ? before : Knob::kRouteSeedAmount;
			ctrl.beginAmountGesture();
			ctrl.setAmount(drag_.anchor);
		}
		else {
			// NO GESTURE YET: each axis opens its own on its first real change (`Knob::commit`'s rule), so a click, or a
			// drag along one axis, never touches the other control's host gesture (touch automation would flatten it).
			drag_.startFeedback = control(static_cast<size_t>(ph::Param::Feedback)).value();
		}
		update();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& ctrl = control(drag_.control);
		if (drag_.route) {
			const float travel = (drag_.startY - e.position.y) / Knob::kRouteDragRange;
			ctrl.setAmount(std::clamp(drag_.anchor + travel, -device::kMaxAmount, device::kMaxAmount));
			update();
			return;
		}
		DeviceControl& fb = control(static_cast<size_t>(ph::Param::Feedback));
		if (e.isShiftDown() != drag_.fine) {
			// RE-ANCHOR: the modifier changes the rate from here on, not the distance already travelled.
			drag_.fine = e.isShiftDown();
			drag_.startX = e.position.x;
			drag_.startY = e.position.y;
			drag_.startValue = ctrl.value();
			drag_.startFeedback = fb.value();
			return;
		}
		// X: written only when it moves the value, its gesture opened on that first move.
		if (const double x = valueForDrag(e.position.x); drag_.xOpen || x != ctrl.value()) {
			if (!drag_.xOpen) {
				drag_.xOpen = true;
				ctrl.beginGesture();
			}
			ctrl.setValue(x);
		}
		// Y: past a dead zone (`DisperserPlot`'s Q road), so a sideways drag never writes Feedback. Opening moves the
		// anchor by the zone, so Feedback starts from where it was rather than jumping by it.
		const float rise = drag_.startY - e.position.y;
		if (!drag_.yOpen && std::abs(rise) > kFeedbackDeadZone) {
			drag_.yOpen = true;
			drag_.startY -= rise < 0.0f ? -kFeedbackDeadZone : kFeedbackDeadZone;
			fb.beginGesture();
		}
		if (drag_.yOpen) {
			fb.setValue(feedbackForDrag(e.position.y));
		}
		update();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& ctrl = control(drag_.control);
		if (drag_.route) {
			const float amount = ctrl.display().amount;
			if (std::abs(amount) < 0.05f && amount != 0.0f) {
				ctrl.setAmount(0.0f);
			}
			ctrl.endAmountGesture();
		}
		else {
			// ONLY THE GESTURES THAT OPENED.
			if (drag_.xOpen) {
				ctrl.endGesture();
			}
			if (drag_.yOpen) {
				control(static_cast<size_t>(ph::Param::Feedback)).endGesture();
			}
		}
		drag_ = {};
	};
	// THE HOVER ROAD, LAST (`tooltip.h`).
	installTooltipHover(*this, *this);
}

DeviceControl& PhaserPlot::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, ph::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

void PhaserPlot::offeredControls(std::vector<Offered>& out) const {
	// SIDE ROADS: x is Center (Phaser) or Delay (Flanger), whichever the Mode reads; y is Feedback.
	out.push_back({ &control(static_cast<size_t>(ph::Param::CenterHz)), Road::Gesture });
	out.push_back({ &control(static_cast<size_t>(ph::Param::DelayMs)), Road::Gesture });
	out.push_back({ &control(static_cast<size_t>(ph::Param::Feedback)), Road::Gesture });
}

void PhaserPlot::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
}

size_t PhaserPlot::draggedControl() const noexcept {
	const bool flanger = std::lround(host_.fieldAt(device_, static_cast<size_t>(ph::Param::Mode)))
	                     == static_cast<long>(ph::Mode::Flanger);
	return static_cast<size_t>(flanger ? ph::Param::DelayMs : ph::Param::CenterHz);
}

bool phaserNoteTracked(const backplane::DeviceHost& host, DeviceInstanceId id) noexcept {
	const auto i = static_cast<size_t>(ph::Reading::NoteHz);
	return host.readingCountAt(id) > i && host.readingAt(id, i) != 0.0f;
}

std::string phaserOffsetText(ph::Mode mode, double knobLog2) {
	const double raw = ph::keyOffsetSemitonesOf(mode, knobLog2);
	const double st = std::fabs(raw) < 0.05 ? 0.0 : raw;
	char out[32];
	std::snprintf(out, sizeof out, st == 0.0 ? "%.1f st" : "%+.1f st", st);
	return out;
}

bool PhaserPlot::tracking() const noexcept { return phaserNoteTracked(host_, device_); }

float PhaserPlot::inset() const noexcept { return large_ ? kInsetLarge : kInsetCard; }

size_t PhaserPlot::wantedPoints() const noexcept {
	// ONE POINT PER PIXEL OF THE CURVE'S WIDTH — logical pixels, which is what a segment is drawn in.
	const float w = width() - 2.0f * inset();
	return std::max(kMinPoints, static_cast<size_t>(std::max(0.0f, std::ceil(w))) + 1);
}

double PhaserPlot::hzAt(size_t i) const noexcept {
	const double t = static_cast<double>(i) / static_cast<double>(std::max<size_t>(dbL_.size() - 1, 1));
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float PhaserPlot::hzToX(double hz) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * inset());
	const double t = (log10Of(hz) - log10Of(kMinHz)) / (log10Of(kMaxHz) - log10Of(kMinHz));
	return inset() + static_cast<float>(std::clamp(t, 0.0, 1.0)) * w;
}

double PhaserPlot::xToHz(float x) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * inset());
	const double t = std::clamp(static_cast<double>((x - inset()) / w), 0.0, 1.0);
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float PhaserPlot::dbToY(float db) const noexcept {
	const float top = inset() + (large_ ? 12.0f : 10.0f);
	const float bottom = height() - inset() - kFootHeight;
	const float t = std::clamp((db - kBottomDb) / (kTopDb - kBottomDb), 0.0f, 1.0f);
	return bottom - t * (bottom - top);
}

float PhaserPlot::sweepToX(double s) const noexcept {
	const ph::Settings& set = drawnSettings_;
	const double sr = modelL_.sampleRate;
	const double noteHz = static_cast<double>(drawnReadings_[static_cast<size_t>(ph::Reading::NoteHz)]);
	const ph::Targets t = ph::targetsOf(set, noteHz, sr);
	if (set.mode == ph::Mode::Phaser) {
		return hzToX(ph::phaserCornerHz(t.centerLog2, s, sr));
	}
	// THE FLANGER, AS THE COMB'S TOOTH SPACING 1/delay — where key tracking puts the note (`keyDelayLog2s`).
	double samples = 0.0;
	if (set.throughZero) {
		const double dry = static_cast<double>(ph::tzDrySamples(sr));
		samples = std::fabs(ph::tzWetOfSpan(dry, ph::tzSpanSamples(t.delayLog2, dry, sr), s) - dry);
	}
	else {
		samples = ph::flangerDelaySamples(t.delayLog2, s, sr);
	}
	return samples > 0.0 ? hzToX(sr / samples) : hzToX(kMaxHz);
}

float PhaserPlot::handleX() const noexcept {
	// THE CENTRE ITSELF — the knob's, or the tracked note's: s = 0 in the Phaser; 1/Delay in the Flanger (under
	// through-zero s = 0 is the crossing, which has no hertz, so the handle sits at the span's, 1/Delay, too).
	const ph::Settings& set = drawnSettings_;
	if (set.mode == ph::Mode::Phaser) {
		return sweepToX(0.0);
	}
	const double sr = modelL_.sampleRate;
	const double noteHz = static_cast<double>(drawnReadings_[static_cast<size_t>(ph::Reading::NoteHz)]);
	const ph::Targets t = ph::targetsOf(set, noteHz, sr);
	return hzToX(1.0 / std::exp2(t.delayLog2));
}

float PhaserPlot::stripeOffset() const noexcept {
	if (!ph::isBarberpole(drawnSettings_.sweep) || reducedMotionRequested()) {
		return 0.0f;
	}
	// THE PUBLISHED RAMP PHASE — Down's ψ already runs backwards (`barberPhaseOf`), so one rule draws both.
	const float psi = drawnReadings_[static_cast<size_t>(ph::Reading::BarberPhase)];
	return (psi - std::floor(psi)) * kStripePeriod;
}

double PhaserPlot::octavesPerPixel() const noexcept {
	const double w = std::max(1.0, static_cast<double>(width() - 2.0f * inset()));
	return std::log2(kMaxHz / kMinHz) / w;
}

float PhaserPlot::feedbackDotY() const noexcept { return feedbackToY(static_cast<double>(drawnSettings_.feedback)); }

float PhaserPlot::feedbackTop() const noexcept { return inset() + (large_ ? 14.0f : 12.0f); }

float PhaserPlot::feedbackFoot() const noexcept { return height() - inset() - kFootHeight - 2.0f; }

float PhaserPlot::feedbackToY(double feedback) const noexcept {
	const float mid = 0.5f * (feedbackTop() + feedbackFoot());
	const float half = std::max(1.0f, 0.5f * (feedbackFoot() - feedbackTop()));
	return mid - static_cast<float>(std::clamp(feedback, -1.0, 1.0)) * half;
}

double PhaserPlot::feedbackPerPixel() const noexcept {
	// A FIXED SPAN, the same feel on the card's short plot as on the window's tall one: the full −1..+1 over
	// `kFeedbackDragSpan` pixels. (The ring is drawn on the plot's own ±1 scale, so on the card it moves a little faster
	// than the hand and in the window a little slower; the VALUE per pixel is what a hand learns, and it is one.)
	return 2.0 / static_cast<double>(kFeedbackDragSpan);
}

double PhaserPlot::feedbackForDrag(float y) const noexcept {
	const double factor = drag_.fine ? static_cast<double>(Knob::kFineFactor) : 1.0;
	return std::clamp(drag_.startFeedback + static_cast<double>(drag_.startY - y) * feedbackPerPixel() * factor, -1.0,
	                  1.0);
}

double PhaserPlot::valueForDrag(float x) const noexcept {
	const double factor = drag_.fine ? static_cast<double>(Knob::kFineFactor) : 1.0;
	const double octaves = static_cast<double>(x - drag_.startX) * octavesPerPixel() * factor;
	if (drag_.control == static_cast<size_t>(ph::Param::CenterHz)) {
		return std::clamp(drag_.startValue + octaves, ph::kCenterLog2Min, ph::kCenterLog2Max);
	}
	// THE FLANGER'S DELAY IS 1/hz on the axis — right is shorter.
	return std::clamp(drag_.startValue - octaves, ph::kDelayLog2Min, ph::kDelayLog2Max);
}

void PhaserPlot::recompute() {
	const double rate = drawnRate_;
	const size_t n = drawnPoints_;
	// SIZED ONLY WHEN THE WIDTH MOVES — the message thread, never a paint.
	dbL_.resize(n);
	dbR_.resize(n);
	loL_.resize(n);
	hiL_.resize(n);
	modelL_ = ph::responseModelOf(drawnSettings_, ph::responseInputsOf(drawnReadings_.data(), drawnReadings_.size(), 0),
	                              rate);
	modelR_ = ph::responseModelOf(drawnSettings_, ph::responseInputsOf(drawnReadings_.data(), drawnReadings_.size(), 1),
	                              rate);
	// A PIXEL'S WIDTH IN LOG2 OF HZ: the sub-samples spread over ±half of it around the pixel's own point.
	const double octavesPerPoint = std::log2(kMaxHz / kMinHz) / static_cast<double>(std::max<size_t>(n - 1, 1));
	for (size_t i = 0; i < n; ++i) {
		const double hz = hzAt(i);
		dbL_[i] = ph::magnitudeDb(modelL_, hz);
		dbR_[i] = ph::magnitudeDb(modelR_, hz);
		float lo = dbL_[i];
		float hi = dbL_[i];
		for (int k = 0; k < kSubSamples; ++k) {
			const double off = (static_cast<double>(k) + 0.5) / kSubSamples - 0.5;
			const float db = ph::magnitudeDb(modelL_, hz * std::exp2(off * octavesPerPoint));
			lo = std::min(lo, db);
			hi = std::max(hi, db);
		}
		loL_[i] = lo;
		hiL_[i] = hi;
	}
	// THE SWEEP'S REACH: Position, ± the LFO's depth while it runs, ± the follower's amount while it runs, ±1 at most.
	// Sampled across the range rather than read at its ends, because under through-zero the hertz is not monotone in s
	// (it runs off the top at the crossing).
	const ph::Settings& s = drawnSettings_;
	const double lfo = s.lfoOn ? static_cast<double>(s.lfoDepth) : 0.0;
	const double env = s.envOn ? std::fabs(static_cast<double>(s.envAmount)) : 0.0;
	position_ = std::clamp(static_cast<double>(s.position), -1.0, 1.0);
	const double lo = std::clamp(position_ - lfo - env, -1.0, 1.0);
	const double hi = std::clamp(position_ + lfo + env, -1.0, 1.0);
	bandX0_ = bandX1_ = sweepToX(lo);
	constexpr int kBandSamples = 32;
	for (int k = 0; k <= kBandSamples; ++k) {
		const double v = lo + (hi - lo) * static_cast<double>(k) / kBandSamples;
		const float x = sweepToX(v);
		bandX0_ = std::min(bandX0_, x);
		bandX1_ = std::max(bandX1_, x);
	}
	if (lo < 0.0 && hi > 0.0) {
		const float x = sweepToX(0.0);
		bandX0_ = std::min(bandX0_, x);
		bandX1_ = std::max(bandX1_, x);
	}
	computed_ = true;
	++recomputes_;
}

void PhaserPlot::resized() {
	// THE CURVE IS ONE POINT PER PIXEL, so a new width is a new curve — taken at the next `update`, never here.
	redraw();
}

bool PhaserPlot::update() {
	const ph::Settings s = phaserSettingsAt(host_, device_, true);
	const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
	// **ONLY THE READINGS THE MODEL CONSUMES, AND ONLY WHILE IT CONSUMES THEM** (the Task 9b review). The engine goes
	// on advancing its LFO's phase and its follower, and publishing its sweep, corners and delays, with both sources
	// switched off — the "sources off, an external modulator on Position" setup the design names — so comparing the
	// whole array recomputed every frame while nothing drawn could move. Every other entry is held at 0 (what
	// `responseInputsOf` reads for an absent one), and so is a consumed one whose term is multiplied by zero:
	//   - LfoPhase while the LFO's amount is nonzero (`targetsOf`: Depth, or 0 with the LFO off). Every use of φ in
	//     `channelSweepOf` — the sweep's shape term and the stages' phase offsets (`offsetSweepOf`) — is scaled by it;
	//   - Env while the follower's amount is nonzero (`envTerm = envAmount · env`);
	//   - BarberPhase under the barberpole, whatever the amounts: the voices' WINDOWS read ψ, and the taps' explore
	//     offsets differ by voice, so ψ moves the response even at zero Depth;
	//   - NoteHz always (key tracking's centre).
	// An entry the host does not publish stays 0, so a short array and a zero-padded one draw the same model.
	std::array<float, ph::kReadingCount> readings {};
	{
		const size_t present = std::min(host_.readingCountAt(device_), readings.size());
		const auto at = [&](ph::Reading r) {
			const auto i = static_cast<size_t>(r);
			return i < present ? host_.readingAt(device_, i) : 0.0f;
		};
		const ph::Targets t = ph::targetsOf(s, static_cast<double>(at(ph::Reading::NoteHz)), rate);
		readings[static_cast<size_t>(ph::Reading::NoteHz)] = at(ph::Reading::NoteHz);
		if (t.lfoAmount != 0.0) {
			readings[static_cast<size_t>(ph::Reading::LfoPhase)] = at(ph::Reading::LfoPhase);
		}
		if (t.envAmount != 0.0) {
			readings[static_cast<size_t>(ph::Reading::Env)] = at(ph::Reading::Env);
		}
		if (ph::isBarberpole(s.sweep)) {
			readings[static_cast<size_t>(ph::Reading::BarberPhase)] = at(ph::Reading::BarberPhase);
		}
	}
	const size_t points = wantedPoints();
	const device::ModDisplay shown = control(draggedControl()).display();
	const bool armed = shown.armed && shown.isDestination;
	const float amount = armed ? shown.amount : 0.0f;

	bool changed = false;
	if (!computed_ || !(s == drawnSettings_) || readings != drawnReadings_
	    || rate != drawnRate_ || points != drawnPoints_) {
		drawnSettings_ = s;
		drawnReadings_ = readings;
		drawnRate_ = rate;
		drawnPoints_ = points;
		recompute();
		changed = true;
	}
	if (armed != drawnArmed_ || amount != drawnAmount_) {
		drawnArmed_ = armed;
		drawnAmount_ = amount;
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

const char* PhaserPlot::tooltipAt(visage::Point) const {
	// TWO CONTROLS ON ONE HANDLE, so it names both axes — as Resonate's plot does — in the mode's words.
	return draggedControl() == static_cast<size_t>(ph::Param::DelayMs) ? kFlangerDragTip : kPhaserDragTip;
}

void PhaserPlot::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	const float in = inset();
	const float foot = h - in - kFootHeight;
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 5.0f, 1.0f);
	if (dbL_.empty()) {
		return;
	}

	// THE GRID: decades of frequency, and 0 dB with steps of 12 under it.
	canvas.setColor(withAlpha(kLine, 0.6f));
	for (const double hz : { 100.0, 1000.0, 10000.0 }) {
		canvas.fill(hzToX(hz), in, 1.0f, foot - in);
	}
	for (float db = kTopDb; db >= kBottomDb; db -= 12.0f) {
		canvas.setColor(withAlpha(kLine, db == 0.0f ? 0.9f : 0.45f));
		canvas.fill(in, dbToY(db), w - 2.0f * in, 1.0f);
	}

	// THE SWEEP BAND, and under the barberpole its stripes, moved by the device's own ramp phase.
	const float bandW = std::max(1.0f, bandX1_ - bandX0_);
	canvas.setColor(withAlpha(kModulation, 0.10f));
	canvas.fill(bandX0_, in, bandW, foot - in);
	if (ph::isBarberpole(drawnSettings_.sweep)) {
		canvas.setColor(withAlpha(kModulation, 0.16f));
		const float offset = stripeOffset();
		for (float x = bandX0_ - kStripePeriod + offset; x < bandX1_; x += kStripePeriod) {
			const float a = std::max(x, bandX0_);
			const float b = std::min(x + kStripeWidth, bandX1_);
			if (b > a) {
				canvas.fill(a, in, b - a, foot - in);
			}
		}
	}

	// THE FLANGER'S UNTRUSTED BAND — the model is the ideal comb up there, not the engine's kernel.
	const bool flanger = modelL_.mode == ph::Mode::Flanger;
	const double trustedHz = ph::kFlangerTrustedFraction * modelL_.sampleRate;
	const float trustedX = flanger && trustedHz < kMaxHz ? hzToX(trustedHz) : w;
	if (trustedX < w - in) {
		canvas.setColor(withAlpha(kSurfaceRaised, 0.55f));
		canvas.fill(trustedX, in, w - in - trustedX, foot - in);
	}

	// THE CURVES: right first and faint, left over it bright — a mono setting shows one line. Dim past the trusted band.
	const auto curve = [&](const std::vector<float>& db, float alpha, float thickness) {
		for (size_t i = 1; i < db.size(); ++i) {
			const float x0 = hzToX(hzAt(i - 1));
			const float x1 = hzToX(hzAt(i));
			canvas.setColor(withAlpha(kAccent, x0 >= trustedX ? alpha * 0.35f : alpha));
			canvas.segment(x0, dbToY(db[i - 1]), x1, dbToY(db[i]), thickness, true);
		}
	};
	// THE LEFT CHANNEL'S SPAN BARS, where a pixel holds more of the response than one point says.
	for (size_t i = 0; i < loL_.size(); ++i) {
		if (hiL_[i] - loL_[i] < kSpanBarDb) {
			continue;
		}
		const float x = hzToX(hzAt(i));
		const float yTop = dbToY(hiL_[i]);
		canvas.setColor(withAlpha(kAccent, x >= trustedX ? 0.12f : 0.32f));
		canvas.fill(x - 0.5f, yTop, 1.0f, std::max(1.0f, dbToY(loL_[i]) - yTop));
	}
	curve(dbR_, 0.35f, 1.0f);
	curve(dbL_, 1.0f, 1.5f);

	// THE MODULATION REACH on the axis, while a source is armed on the centre control.
	const size_t index = draggedControl();
	const device::ModDisplay shown = control(index).display();
	const float hx = handleX();
	if (shown.armed && shown.isDestination && shown.amount != 0.0f) {
		const auto& desc = ph::kParamDescs[index];
		// Both controls are log2 units, so a reach of `amount · range` is that many octaves (negated for the delay).
		const double octaves = static_cast<double>(shown.amount) * (desc.max - desc.min);
		const double hz = xToHz(hx);
		const double sign = index == static_cast<size_t>(ph::Param::DelayMs) ? -1.0 : 1.0;
		const float xUp = hzToX(hz * std::exp2(sign * octaves));
		const float xDown = shown.bipolar ? hzToX(hz * std::exp2(-sign * octaves)) : hx;
		canvas.setColor(withAlpha(kModulation, 0.55f));
		canvas.fill(std::min(xUp, xDown), foot + 1.0f, std::abs(xUp - xDown), 3.0f);
	}

	// THE CENTRE HANDLE — the note's colour while a note is tracked (the knob is then the offset from it).
	const bool noteTracked = tracking();
	canvas.setColor(withAlpha(noteTracked ? kValue : kInk, 0.45f));
	canvas.fill(hx - 0.5f, in, 1.0f, foot - in);
	canvas.setColor(noteTracked ? kValue : kInk);
	canvas.circle(hx - 3.0f, foot - 3.0f, 6.0f);
	// AND ITS SECOND AXIS, FEEDBACK: a faint ±1 scale along the handle's line — short ticks at +1, 0 and −1 — and a
	// ring at the stored Feedback, so the vertical half of the drag has a thing to move.
	{
		canvas.setColor(withAlpha(kInk, 0.3f));
		for (const double v : { 1.0, 0.0, -1.0 }) {
			const float ty = feedbackToY(v);
			const float half = v == 0.0 ? 5.0f : 3.0f;
			canvas.fill(hx - half, ty - 0.5f, 2.0f * half, 1.0f);
		}
		const float fy = feedbackDotY();
		canvas.setColor(noteTracked ? kValue : kInk);
		canvas.ring(hx - 4.5f, fy - 4.5f, 9.0f, 1.5f);
	}

	// THE POSITION TICK at the foot, where the MODULATED Position puts the sweep's centre.
	const float px = sweepToX(position_);
	canvas.setColor(kValue);
	canvas.fill(px - 1.0f, foot + 1.0f, 2.0f, kFootHeight - 1.0f);

	// THE CAPTIONS.
	canvas.setColor(kMuted);
	const char* caption = flanger ? (large_ ? "flanger — response, L bright, R faint; dim past the trusted band"
	                                        : "flanger")
	                              : (large_ ? "phaser — response, L bright, R faint" : "phaser");
	canvas.text(caption, font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, in - 1.0f, w * 0.8f, 11.0f);
	if (large_) {
		char text[16];
		for (const double hz : { 100.0, 1000.0, 10000.0 }) {
			formatHz(text, sizeof(text), hz);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, hzToX(hz) - 30.0f, foot - 12.0f, 60.0f,
			            11.0f);
		}
		canvas.text("0 dB", font(kKnobReadoutSize), visage::Font::kRight, w - in - 40.0f, dbToY(0.0f) - 11.0f, 38.0f,
		            11.0f);
	}
}

} // namespace feathers::ui
