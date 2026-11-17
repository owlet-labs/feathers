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


#include "plugin/ui/card_wheel_gate.h"
#include "plugin/ui/eroder_plot.h"
#include "fx/eroder/loops.h"
#include "fx/eroder/shell.h"
#include "plugin/ui/modulated_row.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

constexpr float kInsetCard = 4.0f;
constexpr float kInsetLarge = 8.0f;
constexpr float kFootHeight = 7.0f;
constexpr float kHandleReach = 10.0f;

constexpr const char* kCombTip =
    "What Tone and Character are turning right now: six quantities ride Tone and five ride Character, each "
    "swept at its own rate";
constexpr const char* kHeadTip = "Where each side's read head has been behind the write, the last few seconds";

struct BarName {
	const char* brief;
	const char* full;
};
constexpr std::array<BarName, EroderPlot::kBars> kBarNames { {
	{ "Dly", "Delay" },
	{ "Fb", "Fdbk" },
	{ "Clp", "Clip" },
	{ "Fld", "Fold" },
	{ "HP", "HP" },
	{ "Wob", "Wob" },
	{ "Shk", "Shake" },
	{ "Res", "Reso" },
	{ "Hld", "Hold" },
	{ "Wid", "Width" },
	{ "Pit", "Pitch" },
} };

void formatHz(char* out, size_t size, double hz) {
	if (hz >= 1000.0) {
		std::snprintf(out, size, "%.1f kHz", hz / 1000.0);
	}
	else {
		std::snprintf(out, size, "%.0f Hz", hz);
	}
}

void formatSeconds(char* out, size_t size, double seconds) {
	if (seconds >= 0.001) {
		std::snprintf(out, size, "%.2f ms", seconds * 1000.0);
	}
	else {
		std::snprintf(out, size, "%.3f ms", seconds * 1000.0);
	}
}

} // namespace

EroderPlot::EroderPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large)
    : host_(host), device_(id), large_(large) {
	controls_.resize(static_cast<size_t>(ero::Param::kCount));
	pre_.fill(feathers::dsp::kAnalyserFloorDb);
	post_.fill(feathers::dsp::kAnalyserFloorDb);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		drag_ = {};
		if (!dust_ || e.position.y > plotFoot() + kFootHeight) {
			return;
		}
		// THE EMPHASIS'S HANDLE (the window's plot only) takes a press nearer it than the band's own: the pointer's pitch moves the
		// offset, its height the amount, both relative to where the press landed.
		if (large_ && std::abs(e.position.x - emphasisHandleX()) <= kEdgeReach
		    && std::abs(e.position.y - emphasisHandleY()) <= kEdgeReach) {
			const float dEmph = std::hypot(e.position.x - emphasisHandleX(), e.position.y - emphasisHandleY());
			const float dBand = etch_ ? 1.0e9f : std::hypot(e.position.x - hzToX(band_.freqHz), e.position.y - handleY());
			if (dEmph < dBand) {
				drag_.active = true;
				drag_.emph = true;
				drag_.startX = e.position.x;
				drag_.startY = e.position.y;
				drag_.startOffset = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::EmphOffset)));
				drag_.startAmount = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::EmphAmount)));
				return;
			}
		}
		// (Etch has no band, so no edge handle: only Freq answers the press.)
		DeviceControl& freq = control(ero::Param::Freq);
		drag_.active = true;
		drag_.startY = e.position.y;
		// THE BAND'S EDGE: a press on it drags Width, across. Only where there is a band (Source above nothing).
		// A press nearer the band's own handle is that handle's.
		if (!etch_ && settings_.source > 0.0f && std::abs(e.position.x - edgeX()) <= kEdgeReach
		    && std::abs(e.position.y - edgeY()) <= kEdgeReach
		    && std::abs(e.position.x - edgeX()) < std::abs(e.position.x - hzToX(band_.freqHz))) {
			drag_.width = true;
			drag_.startX = e.position.x;
			drag_.startWidth = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::Width)));
			control(ero::Param::Width).beginGesture();
			update();
			return;
		}
		if (!e.isAltDown() && freq.display().routable) {
			drag_.route = true;
			const float before = freq.display().amount;
			drag_.anchor = before != 0.0f ? before : Knob::kRouteSeedAmount;
			freq.beginAmountGesture();
			freq.setAmount(drag_.anchor);
		}
		else {
			freq.beginGesture();
			freq.setValue(freqValueForX(e.position.x));
			drag_.startAmount = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::Amount)));
		}
		update();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_.active) {
			return;
		}
		if (drag_.emph) {
			dragEmphasis(e.position);
			return;
		}
		DeviceControl& freq = control(ero::Param::Freq);
		if (drag_.width) {
			// OUTWARD IS WIDER: the whole of Width to a fixed share of the plot, relative to the press.
			const float span = std::max(1.0f, (hzToX(kMaxHz) - hzToX(kMinHz)) * kWidthDragShare);
			control(ero::Param::Width)
			    .setValue(std::clamp(drag_.startWidth + static_cast<double>((e.position.x - drag_.startX) / span), 0.0, 1.0));
			update();
			return;
		}
		if (drag_.route) {
			const float travel = (drag_.startY - e.position.y) / Knob::kRouteDragRange;
			freq.setAmount(std::clamp(drag_.anchor + travel, -device::kMaxAmount, device::kMaxAmount));
		}
		else {
			freq.setValue(freqValueForX(e.position.x));
			// THE AMOUNT: up is more. Relative to the press, past a dead zone, the whole range to the plot's height.
			const float rise = drag_.startY - e.position.y;
			// RUST HAS NO AMOUNT: up and down write nothing there.
			if (!rust_ && !etch_ && (drag_.amountGesture || std::abs(rise) > kAmountDeadZone)) {
				DeviceControl& amount = control(ero::Param::Amount);
				if (!drag_.amountGesture) {
					drag_.amountGesture = true;
					amount.beginGesture();
				}
				const float past = std::max(std::abs(rise) - kAmountDeadZone, 0.0f) * (rise < 0.0f ? -1.0f : 1.0f);
				const double span = ero::kDustAmountLog2Max - ero::kDustAmountLog2Min;
				const double perPixel = span / static_cast<double>(std::max(1.0f, plotFoot() - plotTop()));
				amount.setValue(std::clamp(drag_.startAmount + static_cast<double>(past) * perPixel,
				                           ero::kDustAmountLog2Min, ero::kDustAmountLog2Max));
			}
		}
		update();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_.active) {
			return;
		}
		if (drag_.emph) {
			if (drag_.offsetGesture) {
				control(ero::Param::EmphOffset).endGesture();
			}
			if (drag_.emphAmountGesture) {
				control(ero::Param::EmphAmount).endGesture();
			}
			drag_ = {};
			update();
			return;
		}
		DeviceControl& freq = control(ero::Param::Freq);
		if (drag_.width) {
			control(ero::Param::Width).endGesture();
			drag_ = {};
			update();
			return;
		}
		if (drag_.route) {
			// A route dragged back to nothing is not a route at 0%, it is gone.
			const float amount = freq.display().amount;
			if (std::abs(amount) < 0.05f && amount != 0.0f) {
				freq.setAmount(0.0f);
			}
			freq.endAmountGesture();
		}
		else {
			freq.endGesture();
			if (drag_.amountGesture) {
				control(ero::Param::Amount).endGesture();
			}
		}
		drag_ = {};
		update();
	};
	onMouseWheel() = [this](const visage::MouseEvent& e) {
		if (!cardTakesWheel(*this, e)) {
			return false;
		}
		// WIDTH IS THE NOISE BAND'S: with the shake all sine the wheel has nothing to turn.
		if (!dust_ || etch_ || e.wheel_delta_y == 0.0f || !(settings_.source > 0.0f)) {
			return false;
		}
		DeviceControl& width = control(ero::Param::Width);
		const double now = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::Width)));
		width.beginGesture();
		width.setValue(std::clamp(now + (e.wheel_delta_y > 0.0f ? kWidthPerNotch : -kWidthPerNotch), 0.0, 1.0));
		width.endGesture();
		update();
		return true;
	};
	// THE HOVER ROAD, LAST — `+=` beside the handlers above (`tooltip.h` says why it must come last).
	installTooltipHover(*this, *this);
}

DeviceControl& EroderPlot::control(ero::Param p) const {
	const auto index = static_cast<size_t>(p);
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, ero::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

void EroderPlot::offeredControls(std::vector<Offered>& out) const {
	// A SIDE ROAD for each: x is Freq, y is Amount, the wheel is Width.
	out.push_back({ &control(ero::Param::Freq), Road::Gesture });
	out.push_back({ &control(ero::Param::Amount), Road::Gesture });
	out.push_back({ &control(ero::Param::Width), Road::Gesture });
	// THE EMPHASIS'S HANDLE, the window's plot only: x is the offset, y the amount.
	if (large_) {
		out.push_back({ &control(ero::Param::EmphOffset), Road::Gesture });
		out.push_back({ &control(ero::Param::EmphAmount), Road::Gesture });
	}
}

void EroderPlot::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (const ero::Param p : { ero::Param::Freq, ero::Param::Amount, ero::Param::Width, ero::Param::EmphOffset, ero::Param::EmphAmount }) {
		control(p).setModulation(mod, history);
	}
}

float EroderPlot::inset() const noexcept { return large_ ? kInsetLarge : kInsetCard; }

float EroderPlot::plotTop() const noexcept { return inset() + (large_ ? 12.0f : 10.0f); }

float EroderPlot::plotFoot() const noexcept {
	return height() - inset() - kFootHeight - (large_ ? kHeadStripHeight + 6.0f : 0.0f);
}

float EroderPlot::hzToX(double hz) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * inset());
	const double t = std::log(std::max(hz, 1e-9) / kMinHz) / std::log(kMaxHz / kMinHz);
	return inset() + static_cast<float>(std::clamp(t, 0.0, 1.0)) * w;
}

double EroderPlot::xToHz(float x) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * inset());
	const double t = std::clamp(static_cast<double>((x - inset()) / w), 0.0, 1.0);
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float EroderPlot::dbToY(float db) const noexcept {
	const float t = std::clamp((db - kFootDb) / (kTopDb - kFootDb), 0.0f, 1.0f);
	return plotFoot() - t * (plotFoot() - plotTop());
}

float EroderPlot::handleY() const noexcept {
	// DUST: the Amount's height. RUST has no Amount; its band stands at a fixed height.
	if (rust_) {
		return plotFoot() - kRustBandShare * (plotFoot() - plotTop());
	}
	return amountToY(static_cast<double>(settings_.amount));
}

float EroderPlot::amountToY(double log2Seconds) const noexcept {
	const double t = std::clamp((log2Seconds - ero::kDustAmountLog2Min)
	                                / (ero::kDustAmountLog2Max - ero::kDustAmountLog2Min),
	                            0.0, 1.0);
	return plotFoot() - static_cast<float>(t) * (plotFoot() - plotTop());
}

double EroderPlot::edgeHz() const noexcept {
	// THE CURVE FALLS FROM ITS CENTRE: bisect, in octaves, for where it has fallen `kEdgeDb`.
	double low = 0.0;
	double high = 12.0;
	for (int i = 0; i < 40; ++i) {
		const double mid = 0.5 * (low + high);
		if (bandDb(band_.freqHz * std::exp2(mid)) > kEdgeDb) {
			low = mid;
		}
		else {
			high = mid;
		}
	}
	return band_.freqHz * std::exp2(0.5 * (low + high));
}

float EroderPlot::edgeX() const noexcept {
	return hzToX(std::min(edgeHz(), kMaxHz));
}

float EroderPlot::edgeY() const noexcept {
	const float foot = plotFoot();
	const float rise = foot - handleY();
	const auto level = static_cast<float>(std::clamp(1.0 - kEdgeDb / static_cast<double>(kBandFloorDb), 0.0, 1.0));
	return foot - rise * level;
}

double EroderPlot::bandDb(double hz) const noexcept {
	// THE ENGINE'S FILTER: a trapezoidal bandpass is its analogue prototype at a WARPED frequency, tan(pi f / rate)
	// against the centre's own g. Two of them in series, so the level in dB is twice one's.
	const double warped = std::tan(std::numbers::pi * std::min(hz, 0.499 * rate_) / rate_) / std::max(band_.g, 1e-12);
	const double off = 1.0 - warped * warped;
	const double damp = warped * band_.k;
	const double one = damp * damp / std::max(off * off + damp * damp, 1e-30);
	return 2.0 * 10.0 * std::log10(std::max(one, 1e-30));
}

std::optional<double> EroderPlot::emphasisDb(double hz) const noexcept {
	if (!dust_ || !(settings_.emphAmount != 0.0f) || !std::isfinite(settings_.emphAmount)) {
		return std::nullopt;
	}
	// THE ENGINE'S OWN CLAMPS (`EmphasisPair::set`), then its own sections.
	const double db = std::clamp(static_cast<double>(settings_.emphAmount), -ero::kEmphasisMaxDb, ero::kEmphasisMaxDb);
	const double centre = std::clamp(emphasisHz(), 5.0, rate_ * 0.45);
	const double w = 2.0 * std::numbers::pi * std::clamp(hz, 1.0, 0.499 * rate_) / rate_;
	if (settings_.emphBell) {
		const double octaves =
		    std::clamp(ero::SharedSection::emphasisWidthOctaves(settings_.width), 0.1, 6.0);
		const auto bell = ero::EmphasisPair::bellSection(db, centre, octaves, rate_, false);
		return 20.0 * std::log10(std::max(std::abs(bell.response(w)), 1e-12));
	}
	const auto t = fx::shaper::tiltSection(db, centre, rate_, false);
	const std::complex<double> z1 = std::polar(1.0, -w);
	const std::complex<double> h = (static_cast<double>(t.b0) + static_cast<double>(t.b1) * z1)
	                               / (1.0 + static_cast<double>(t.a1) * z1);
	return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
}

double EroderPlot::emphasisHz() const noexcept {
	// THE ENGINE'S OWN: Freq moved by the offset (`SharedSection::emphasisHz`), not restated here.
	return ero::SharedSection::emphasisHz(band_.freqHz, static_cast<double>(settings_.emphOffset), rate_);
}

float EroderPlot::emphasisHandleX() const noexcept { return hzToX(emphasisHz()); }

float EroderPlot::emphasisHandleY() const noexcept {
	// ON THE CENTRE LINE AT 0 dB, so it can be picked up; otherwise ON THE CURVE, at the emphasis's own tuning.
	const double db = emphasisDb(emphasisHz()).value_or(0.0);
	return std::clamp(emphasisY(db), plotTop(), plotFoot());
}

void EroderPlot::dragEmphasis(visage::Point at) {
	const float dx = at.x - drag_.startX;
	const float rise = drag_.startY - at.y;
	// ACROSS: the offset, in semitones by the plot's own pitch axis, relative to the press and past the dead zone.
	if (drag_.offsetGesture || std::abs(dx) > kAmountDeadZone) {
		DeviceControl& offset = control(ero::Param::EmphOffset);
		if (!drag_.offsetGesture) {
			drag_.offsetGesture = true;
			offset.beginGesture();
		}
		const float past = std::max(std::abs(dx) - kAmountDeadZone, 0.0f) * (dx < 0.0f ? -1.0f : 1.0f);
		const double semitones = 12.0 * std::log2(xToHz(drag_.startX + past) / xToHz(drag_.startX));
		offset.setValue(std::clamp(drag_.startOffset + semitones, -ero::kEmphOffsetSemitones, ero::kEmphOffsetSemitones));
	}
	// UP AND DOWN: the amount, the plot's quarter-height being `kEmphasisDbPerQuarter`, relative and past the dead zone.
	if (drag_.emphAmountGesture || std::abs(rise) > kAmountDeadZone) {
		DeviceControl& amount = control(ero::Param::EmphAmount);
		if (!drag_.emphAmountGesture) {
			drag_.emphAmountGesture = true;
			amount.beginGesture();
		}
		const float past = std::max(std::abs(rise) - kAmountDeadZone, 0.0f) * (rise < 0.0f ? -1.0f : 1.0f);
		const double dbPerPixel = kEmphasisDbPerQuarter / (0.25 * static_cast<double>(std::max(1.0f, plotFoot() - plotTop())));
		amount.setValue(std::clamp(drag_.startAmount + static_cast<double>(past) * dbPerPixel, -ero::kEmphasisMaxDb,
		                           ero::kEmphasisMaxDb));
	}
	update();
}

float EroderPlot::emphasisY(double db) const noexcept {
	// ZERO dB IS THE PLOT'S MIDDLE; +-kEmphasisDbPerQuarter is a quarter of its height either way.
	const float mid = 0.5f * (plotTop() + plotFoot());
	return mid - static_cast<float>(db / kEmphasisDbPerQuarter) * 0.25f * (plotFoot() - plotTop());
}

double EroderPlot::pitchRatio() const noexcept {
	return settings_.track ? std::exp2((pitch_ - ero::kReferenceNote) / 12.0) : 1.0;
}

double EroderPlot::freqValueForX(float x) const noexcept {
	return std::clamp(std::log2(xToHz(x) / pitchRatio()), ero::kDustFreqLog2Min, ero::kDustFreqLog2Max);
}

bool EroderPlot::update() {
	ero::Row row {};
	const size_t present = readModulatedRow(host_, device_, ero::kDescriptor, row);
	const size_t count = host_.readingCountAt(device_);
	const bool publishing = count >= ero::kReadingCount;
	const auto at = [&](ero::Reading r) { return host_.readingAt(device_, static_cast<size_t>(r)); };
	const double pitch = publishing ? static_cast<double>(at(ero::Reading::PitchSemitones)) : ero::kReferenceNote;
	const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;

	bool changed = false;
	if (!computed_ || row != row_ || pitch != pitch_ || rate != rate_) {
		row_ = row;
		settings_ = ero::stateOf(row, present).settings;
		// DUST AND RUST DRAW THE BAND (one moves a head by it, the other multiplies by it); the Comb its bars.
		dust_ = settings_.mode != ero::Mode::Comb;
		rust_ = settings_.mode == ero::Mode::Rust;
		// ETCH DRAWS NO BAND: the spectra, the emphasis, and a marker at Freq.
		etch_ = settings_.mode == ero::Mode::Etch;
		pitch_ = pitch;
		rate_ = rate;
		band_ = ero::deriveDust(settings_, pitch_, rate_);
		const ero::Phases phases = ero::phasesOf(static_cast<double>(settings_.tone),
		                                         static_cast<double>(settings_.character),
		                                         static_cast<double>(settings_.gamma));
		comb_ = ero::derive(phases, static_cast<double>(settings_.cutoffOctaves), static_cast<double>(settings_.mix), pitch_,
		                    rate_);
		const ero::ToneBank tone = ero::evalToneBank(phases.tone);
		const ero::CharacterBank ch = ero::evalCharacterBank(phases.character);
		const auto floorAt = static_cast<float>(ero::kBankFloor);
		bars_ = { std::max(floorAt, tone[0]), std::max(floorAt, tone[1]), tone[2], tone[3], tone[4], tone[5],
			      std::max(static_cast<float>(ero::kDepthFloor), ch[0]), ch[1], ch[2], ch[3], ch[4] };
		computed_ = true;
		++recomputes_;
		changed = true;
	}
	// THE DEVICE COMPUTES THE SPECTRUM ONLY WHILE A VIEW ASKS (`ReadingsSink::wanted`), and only Dust's picture draws it.
	if (dust_ && isVisible()) {
		host_.wantReadings(device_);
	}

	// THE SPECTRUM: a frame is taken only whole (the counter both ends) and only when it is a new one.
	if (publishing) {
		const float a = at(ero::Reading::FrameA);
		if (a != frame_ && a > 0.0f) {
			Bins pre {};
			Bins post {};
			for (size_t k = 0; k < pre.size(); ++k) {
				pre[k] = host_.readingAt(device_, static_cast<size_t>(ero::Reading::PreFirst) + k);
				post[k] = host_.readingAt(device_, static_cast<size_t>(ero::Reading::PostFirst) + k);
			}
			if (at(ero::Reading::FrameB) == a) {
				pre_ = pre;
				post_ = post;
				frame_ = a;
				haveSpectrum_ = true;
				changed = true;
			}
		}
		// THE HEAD: one column each time the device says something new.
		if (large_) {
			const Head now { at(ero::Reading::HeadNearL), at(ero::Reading::HeadFarL), at(ero::Reading::HeadNearR),
				             at(ero::Reading::HeadFarR) };
			if (!(now == lastHead_)) {
				lastHead_ = now;
				head_[headNext_] = now;
				headNext_ = (headNext_ + 1) % head_.size();
				headCount_ = std::min(headCount_ + 1, head_.size());
				changed = true;
			}
		}
	}

	const device::ModDisplay shown = control(ero::Param::Freq).display();
	const bool armed = shown.armed && shown.isDestination;
	const float amount = armed ? shown.amount : 0.0f;
	if (armed != armed_ || amount != routeAmount_ || shown.bipolar != routeBipolar_) {
		armed_ = armed;
		routeAmount_ = amount;
		routeBipolar_ = shown.bipolar;
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

const char* EroderPlot::tooltipAt(visage::Point p) const {
	if (large_ && p.y > plotFoot() + kFootHeight) {
		return kHeadTip;
	}
	if (!dust_) {
		return kCombTip;
	}
	if (large_ && std::abs(p.x - emphasisHandleX()) < kHandleReach && std::abs(p.y - emphasisHandleY()) < kHandleReach) {
		return tooltipOf(&control(ero::Param::EmphOffset));
	}
	// THE HANDLE SPEAKS FOR THE AMOUNT, the rest of the axis for Freq.
	if (!etch_ && std::abs(p.x - hzToX(band_.freqHz)) < kHandleReach
	    && std::abs(p.y - handleY()) < kHandleReach) {
		return tooltipOf(&control(ero::Param::Amount));
	}
	return tooltipOf(&control(ero::Param::Freq));
}

void EroderPlot::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), 5.0f, 1.0f);
	if (dust_) {
		drawDust(canvas);
	}
	else {
		drawComb(canvas);
	}
	if (large_) {
		drawHead(canvas);
	}
}

void EroderPlot::drawDust(visage::Canvas& canvas) {
	const float w = width();
	const float in = inset();
	const float top = plotTop();
	const float foot = plotFoot();

	canvas.setColor(withAlpha(kLine, 0.6f));
	for (const double hz : { 100.0, 1000.0, 10000.0 }) {
		canvas.fill(hzToX(hz), in, 1.0f, foot - in);
	}

	// THE SPECTRA: what came in as a grey fill, what went out as a line, and the difference lit where it is more.
	if (haveSpectrum_) {
		const auto tilted = [](float db, double hz) {
			return db + EroderPlot::kTiltDbPerOctave * static_cast<float>(std::log2(hz / 1000.0));
		};
		float prevX = 0.0f;
		float prevY = 0.0f;
		for (size_t k = 0; k < pre_.size(); ++k) {
			const double hz = feathers::dsp::analyserBinHz(k);
			const float x = hzToX(hz);
			const float yIn = dbToY(tilted(pre_[k], hz));
			const float yOut = dbToY(tilted(post_[k], hz));
			const float step = std::max(1.5f, (w - 2.0f * in) / static_cast<float>(pre_.size()));
			canvas.setColor(withAlpha(kMuted, 0.35f));
			canvas.fill(x - step * 0.5f, yIn, step, std::max(0.0f, foot - yIn));
			if (yOut < yIn) {
				canvas.setColor(withAlpha(kAccent, 0.35f));
				canvas.fill(x - step * 0.5f, yOut, step, yIn - yOut);
			}
			if (k > 0) {
				canvas.setColor(withAlpha(kAccent, 0.9f));
				canvas.segment(prevX, prevY, x, yOut, 1.2f, true);
			}
			prevX = x;
			prevY = yOut;
		}
	}

	// THE EMPHASIS'S HANDLE (window only), whenever the plot is a spectrum: on the centre line at 0 dB, a hollow ring; on the curve's
	// own height when there is an emphasis, a filled one. It is drawn after the curve, below.
	// THE EMPHASIS, where there is one: a thin curve across the axis (the filter in front of the engine), on its own colour.
	if (emphasisDb(1000.0).has_value()) {
		canvas.setColor(withAlpha(tokens::kGammaAxis, 0.35f));
		canvas.fill(in, emphasisY(0.0), w - 2.0f * in, 1.0f);
		canvas.setColor(withAlpha(tokens::kGammaAxis, 0.95f));
		const int points = large_ ? 160 : 72;
		float prevX = 0.0f;
		float prevY = 0.0f;
		for (int i = 0; i <= points; ++i) {
			const double hz = kMinHz * std::pow(kMaxHz / kMinHz, static_cast<double>(i) / points);
			const float x = hzToX(hz);
			const float y = std::clamp(emphasisY(*emphasisDb(hz)), top, foot);
			if (i > 0) {
				canvas.segment(prevX, prevY, x, y, 1.0f, true);
			}
			prevX = x;
			prevY = y;
		}
		if (large_) {
			char label[40];
			std::snprintf(label, sizeof(label), "emphasis %+.1f dB", static_cast<double>(settings_.emphAmount));
			canvas.setColor(tokens::kGammaAxis);
			canvas.text(label, font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, in + 11.0f, 120.0f, 11.0f);
		}
	}
	if (large_) {
		const float hx = emphasisHandleX();
		const float hy = emphasisHandleY();
		const bool on = emphasisDb(1000.0).has_value();
		canvas.setColor(on ? tokens::kGammaAxis : withAlpha(tokens::kGammaAxis, 0.6f));
		canvas.circle(hx - 4.0f, hy - 4.0f, 8.0f);
		canvas.setColor(kSurface);
		canvas.circle(hx - (on ? 2.0f : 2.5f), hy - (on ? 2.0f : 2.5f), on ? 4.0f : 5.0f);
		if (settings_.emphOffset != 0.0f) {
			char label[24];
			std::snprintf(label, sizeof(label), "%+.1f st", static_cast<double>(settings_.emphOffset));
			canvas.setColor(tokens::kGammaAxis);
			canvas.text(label, font(kKnobReadoutSize), visage::Font::kCenter, hx - 30.0f, hy - 17.0f, 60.0f, 11.0f);
		}
	}

	// THE BAND: the noise half as its filter's curve, the sine half as a stem, each as strong as Source makes it.
	const float handleX = hzToX(band_.freqHz);
	const float handleY = this->handleY();
	const float rise = foot - handleY;
	// (Etch has no noise to show, whatever the hidden Source holds.)
	const float noise = etch_ ? 0.0f : std::clamp(settings_.source, 0.0f, 1.0f);
	if (noise > 0.0f && rise > 0.0f) {
		canvas.setColor(withAlpha(kValue, 0.35f + 0.65f * noise));
		const int points = large_ ? 220 : 96;
		float prevX = 0.0f;
		float prevY = 0.0f;
		for (int i = 0; i <= points; ++i) {
			const double hz = kMinHz * std::pow(kMaxHz / kMinHz, static_cast<double>(i) / points);
			const auto level = static_cast<float>(std::clamp(1.0 - bandDb(hz) / static_cast<double>(kBandFloorDb), 0.0, 1.0));
			const float x = hzToX(hz);
			const float y = foot - rise * level;
			if (i > 0) {
				canvas.segment(prevX, prevY, x, y, 1.5f, true);
			}
			prevX = x;
			prevY = y;
		}
	}
	if (etch_) {
		// ETCH'S MARKER AT FREQ: a thin line the plot's height, where the inner loop's delay and the emphasis pivot.
		canvas.setColor(withAlpha(settings_.track ? kValue : kInk, 0.6f));
		canvas.fill(handleX - 0.5f, top, 1.0f, std::max(0.0f, foot - top));
	}
	else if (noise < 1.0f) {
		canvas.setColor(withAlpha(kValue, 0.35f + 0.65f * (1.0f - noise)));
		canvas.fill(handleX - 0.75f, handleY, 1.5f, std::max(0.0f, rise));
	}

	// THE BAND'S EDGE, where it is 12 dB down above its centre: the handle Width is dragged by.
	if (noise > 0.0f && rise > 0.0f) {
		canvas.setColor(withAlpha(kValue, 0.35f + 0.65f * noise));
		canvas.circle(edgeX() - 2.5f, edgeY() - 2.5f, 5.0f);
	}

	// THE MODULATION REACH on Freq, along the foot, in octaves.
	if (armed_ && routeAmount_ != 0.0f) {
		const auto& desc = ero::kParamDescs[static_cast<size_t>(ero::Param::Freq)];
		const double octaves = static_cast<double>(routeAmount_) * (desc.max - desc.min);
		const float xUp = hzToX(band_.freqHz * std::exp2(octaves));
		const float xDown = routeBipolar_ ? hzToX(band_.freqHz * std::exp2(-octaves)) : handleX;
		canvas.setColor(withAlpha(kModulation, 0.55f));
		canvas.fill(std::min(xUp, xDown), foot + 1.0f, std::abs(xUp - xDown), 3.0f);
	}

	// THE HANDLE: a tick on the foot and the dot at the Amount's height — the note's colour while it is tracked.
	canvas.setColor(settings_.track ? kValue : kInk);
	canvas.fill(handleX - 3.0f, foot - 1.0f, 6.0f, 2.0f);
	if (!etch_) {
		canvas.circle(handleX - 3.5f, handleY - 3.5f, 7.0f);
	}

	char text[40];
	canvas.setColor(kMuted);
	canvas.text(etch_ ? large_ ? "in (grey), out (line), the emphasis, and Freq" : "spectrum + Freq"
	            : large_ ? rust_ ? "in (grey), out (line), and the band that multiplies it"
	                   : "in (grey), out (line), and the band that shakes the head" : "spectrum + band",
	            font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, in - 1.0f, w * 0.75f, 11.0f);
	if (!rust_ && !etch_) {
		formatSeconds(text, sizeof(text), band_.amountSeconds);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight, w - in - 70.0f, in - 1.0f, 68.0f, 11.0f);
	}
	if (large_) {
		for (const double hz : { 100.0, 1000.0, 10000.0 }) {
			formatHz(text, sizeof(text), hz);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, hzToX(hz) - 30.0f, foot - 12.0f, 60.0f,
			            11.0f);
		}
		formatHz(text, sizeof(text), band_.freqHz);
		const bool left = handleX > w * 0.75f;
		canvas.setColor(withAlpha(kSurface, 0.9f));
		const float labelY = etch_ ? top + 2.0f : handleY - 6.0f;
		canvas.roundedRectangle(left ? handleX - 62.0f : handleX + 6.0f, labelY, 56.0f, 12.0f, 3.0f);
		canvas.setColor(settings_.track ? kValue : kInk);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, left ? handleX - 62.0f : handleX + 6.0f,
		            labelY, 56.0f, 12.0f);
	}
}

void EroderPlot::drawComb(visage::Canvas& canvas) {
	const float w = width();
	const float in = inset();
	const float top = plotTop() + 2.0f;
	const float labels = large_ ? 26.0f : 11.0f;
	const float foot = plotFoot() + kFootHeight - labels;
	const float groupGap = large_ ? 18.0f : 8.0f;
	const float cell = (w - 2.0f * in - groupGap) / static_cast<float>(kBars);
	const float barW = std::max(2.0f, cell * (large_ ? 0.5f : 0.6f));

	canvas.setColor(kMuted);
	canvas.text("TONE", font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, in - 1.0f, 60.0f, 11.0f);
	canvas.text("CHARACTER", font(kKnobReadoutSize), visage::Font::kLeft,
	            in + 2.0f + cell * static_cast<float>(kToneBars) + groupGap, in - 1.0f, 90.0f, 11.0f);

	char text[32];
	for (size_t i = 0; i < kBars; ++i) {
		const float x = in + cell * static_cast<float>(i) + (i >= kToneBars ? groupGap : 0.0f);
		const float barX = x + (cell - barW) * 0.5f;
		const float tall = std::max(0.0f, foot - top);
		canvas.setColor(withAlpha(kLine, 0.8f));
		canvas.fill(barX, top, barW, tall);
		canvas.setColor(i < kToneBars ? kAccent : kValue);
		const float fill = tall * std::clamp(bars_[i], 0.0f, 1.0f);
		canvas.fill(barX, foot - fill, barW, fill);
		canvas.setColor(kMuted);
		canvas.text(large_ ? kBarNames[i].full : kBarNames[i].brief, font(kKnobReadoutSize), visage::Font::kCenter,
		            x - 8.0f, foot + 1.0f, cell + 16.0f, 11.0f);
		if (large_) {
			switch (i) {
			case 0: formatSeconds(text, sizeof(text), comb_.baseDelaySeconds); break;
			case 1: std::snprintf(text, sizeof(text), "%.0f%%", comb_.feedback * 100.0); break;
			case 2: std::snprintf(text, sizeof(text), "x%.1f", comb_.clipIntensity > 0.0 ? comb_.clipDrive : 0.0); break;
			case 3: std::snprintf(text, sizeof(text), "x%.1f", comb_.foldIntensity > 0.0 ? comb_.foldDrive : 0.0); break;
			case 4: formatHz(text, sizeof(text), comb_.hpfHz); break;
			case 5: std::snprintf(text, sizeof(text), "%.0f%%", comb_.triangleDepth * 100.0); break;
			case 6: std::snprintf(text, sizeof(text), "%.0f%%", comb_.depth * 100.0); break;
			case 7: std::snprintf(text, sizeof(text), "%.0f%%", comb_.resonance * 100.0); break;
			case 8: std::snprintf(text, sizeof(text), "%.0f%%", comb_.shBlend * 100.0); break;
			case 9: std::snprintf(text, sizeof(text), "%.0f%%", comb_.width * 100.0); break;
			default: formatHz(text, sizeof(text), comb_.triangleHz); break;
			}
			canvas.setColor(kInk);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, x - 8.0f, foot + 13.0f, cell + 16.0f,
			            11.0f);
		}
	}
}

void EroderPlot::drawHead(visage::Canvas& canvas) {
	const float in = inset();
	const float stripTop = height() - in - kHeadStripHeight;
	const float w = width() - 2.0f * in;
	canvas.setColor(withAlpha(kLine, 0.8f));
	canvas.fill(in, stripTop - 3.0f, w, 1.0f);
	canvas.setColor(kMuted);
	canvas.text("READ HEAD", font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, stripTop, 80.0f, 11.0f);
	if (headCount_ == 0) {
		canvas.text("no signal yet", font(kKnobReadoutSize), visage::Font::kLeft, in + 2.0f, stripTop + 16.0f, 120.0f,
		            11.0f);
		return;
	}
	float farthest = 1.0e-6f;
	for (size_t i = 0; i < headCount_; ++i) {
		farthest = std::max({ farthest, head_[i].farL, head_[i].farR });
	}
	char text[32];
	formatSeconds(text, sizeof(text), static_cast<double>(farthest));
	canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight, in + w - 72.0f, stripTop, 70.0f, 11.0f);
	// TWO LANES, left over right; the newest column at the right edge.
	const float laneTop = stripTop + 12.0f;
	const float laneH = (kHeadStripHeight - 14.0f) * 0.5f;
	const float column = w / static_cast<float>(head_.size());
	for (size_t n = 0; n < headCount_; ++n) {
		const Head& h = head_[(headNext_ + head_.size() - 1 - n) % head_.size()];
		const float x = in + w - column * static_cast<float>(n + 1);
		const auto lane = [&](float nearAt, float farAt, float y, uint32_t colour) {
			const float lo = y + laneH * (1.0f - std::clamp(farAt / farthest, 0.0f, 1.0f));
			const float hi = y + laneH * (1.0f - std::clamp(nearAt / farthest, 0.0f, 1.0f));
			canvas.setColor(colour);
			canvas.fill(x, lo, std::max(1.0f, column), std::max(1.0f, hi - lo));
		};
		lane(h.nearL, h.farL, laneTop, kModulationL);
		lane(h.nearR, h.farR, laneTop + laneH + 1.0f, kModulationR);
	}
}

} // namespace feathers::ui
