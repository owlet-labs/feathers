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

#include "plugin/ui/delay_expanded.h"

#include "fx/delay/delay_lanes.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace feathers::ui {

namespace dl = fx::delay;

namespace {

rack::InnerChain* laneOf(void* surface) {
	return dl::feedbackLaneOf(surface);
}

/// THE CARD'S CAPTION FOR A CONTROL: its declared short name, or its name without the device's (`GenericCard`).
std::string captionOf(dl::Param p) {
	// FEEDBACK'S SHORT NAME IS "Amount", which reads under the card's FEEDBACK heading and nowhere else: here it sits in
	// the LOOP band beside Tone, so it says what it is.
	if (p == dl::Param::Feedback) {
		return "Feedback";
	}
	const device::ParamDesc& d = dl::kParamDescs[static_cast<size_t>(p)];
	if (d.shortName != nullptr && d.shortName[0] != '\0') {
		return d.shortName;
	}
	const std::string name = d.name == nullptr ? std::string {} : d.name;
	const std::string prefix = std::string(dl::kTraits.name) + " ";
	return name.rfind(prefix, 0) == 0 && name.size() > prefix.size() ? name.substr(prefix.size()) : name;
}

/// THE MODE BAND'S FOUR, per mode — the card's flex row, in the card's order.
constexpr dl::Param kGrainFour[] = { dl::Param::GrainSize, dl::Param::Density, dl::Param::Spray, dl::Param::Pitch };
constexpr dl::Param kTapeFour[] = { dl::Param::Wow, dl::Param::Flutter, dl::Param::Saturation, dl::Param::HfLoss };
constexpr dl::Param kTapsFour[] = { dl::Param::Taps, dl::Param::TapPattern, dl::Param::TapLevels, dl::Param::TapWidth };
/// THE BUCKET BRIGADE'S FOUR (2026-11-05), the band's fourth tenant; and Swarm, a fifth cell on the grain band.
constexpr dl::Param kBbdFour[] = { dl::Param::BbdStages, dl::Param::BbdCompander, dl::Param::BbdNoise, dl::Param::BbdDrive };

/// THE CELL HEIGHT OF A BAND ROW: a value box, or a zone cell's row at the Engine rail's diameter, whichever is taller.
float bandRowHeight() {
	return std::max(ValueBox::kHeight, Knob::rowHeight(Knob::kEngineRailDiameter));
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE TIME PLANE                                                                                     //
//--------------------------------------------------------------------------------------------------//

DelayPlane::DelayPlane(backplane::DeviceHost& host, DeviceInstanceId id, ControlOf control)
    : host_(host), id_(id), control_(std::move(control)) {
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { move(e.position); };
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	installTooltipHover(*this, *this); // LAST: an `=` after it would drop the hooks (tooltip.h)
}

bool DelayPlane::live(dl::Param p) const {
	const device::ParamDesc& d = dl::kParamDescs[static_cast<size_t>(p)];
	return d.gateControl < 0 || device::relevantFor(d, host_.fieldAt(id_, static_cast<size_t>(d.gateControl)));
}

double DelayPlane::fieldOf(dl::Param p) const { return static_cast<double>(host_.fieldAt(id_, static_cast<size_t>(p))); }

double DelayPlane::handLeftTap(const dl::TapPicture& p) const { return p.axisUnit * std::exp2(fieldOf(dl::Param::Time)); }

DelayStrip DelayPlane::strip() const {
	if (drag_.handle != Handle::None) {
		DelayStrip held = drag_.strip;
		held.bounds = visage::Bounds { 0.0f, 0.0f, width(), height() };
		return held;
	}
	dl::TapPicture p;
	const bool has = delayPicture(host_, id_, &p);
	return { visage::Bounds { 0.0f, 0.0f, width(), height() },
		     has ? dl::stripSpanSeconds(delayHandUnitSeconds(host_, id_, p)) : 0.0 };
}

std::vector<dl::StripTick> DelayPlane::ticksForTest() const {
	dl::TapPicture p;
	if (!delayPicture(host_, id_, &p)) {
		return {};
	}
	return dl::stripTicks(p, strip().span);
}

void DelayPlane::offeredControls(std::vector<Offered>& out) const {
	for (const dl::Param p : { dl::Param::Time, dl::Param::Spread, dl::Param::Feedback, dl::Param::Tone, dl::Param::Taps,
	                           dl::Param::TapPattern, dl::Param::TapLevels }) {
		out.push_back({ &control_(p), Road::Gesture });
	}
}

bool DelayPlane::handleAt(Handle h, visage::Point* out) const {
	dl::TapPicture p;
	if (!delayPicture(host_, id_, &p)) {
		return false;
	}
	const DelayStrip s = strip();
	const double unit = handLeftTap(p);
	const float first = std::max(p.wetGain, dl::kStripGainFloor * 2.0f);
	const bool multi = p.layout == dl::TapLayout::MultiTap;
	visage::Point at {};
	switch (h) {
	case Handle::Time:
		if (!live(dl::Param::Time)) {
			return false;
		}
		at = { s.xAt(unit), s.tipY(0, first) };
		break;
	case Handle::Spread:
		if (!live(dl::Param::Spread)) {
			return false;
		}
		at = { s.xAt(unit * (1.0 + dl::kSpreadFraction * std::clamp(fieldOf(dl::Param::Spread), 0.0, 1.0))),
			   s.tipY(1, first) };
		break;
	case Handle::Feedback:
		if (!live(dl::Param::Feedback)) {
			return false;
		}
		at = { s.xAt(2.0 * unit), s.tipY(0, std::max(first * p.feedback, dl::kStripGainFloor * 1.01f)) };
		break;
	case Handle::Tone:
		if (!live(dl::Param::Tone)) {
			return false;
		}
		at = { width() - DelayStrip::kInsetRight - 10.0f,
			   s.midY() - static_cast<float>(fieldOf(dl::Param::Tone)) * (height() * 0.5f - 12.0f) };
		break;
	case Handle::Taps: {
		if (!multi || !live(dl::Param::Taps)) {
			return false;
		}
		double earliest = 1.0;
		for (int k = 0; k < p.tapCount; ++k) {
			earliest = std::min(earliest, p.tapFrac[static_cast<size_t>(k)]);
		}
		at = { s.xAt(unit * earliest), s.midY() };
		break;
	}
	case Handle::Cluster: {
		if (!multi || !live(dl::Param::TapPattern)) {
			return false;
		}
		const dl::TapZone pattern = dl::decodeTapCoord(static_cast<float>(fieldOf(dl::Param::TapPattern)));
		const dl::TapZone levels = dl::decodeTapCoord(static_cast<float>(fieldOf(dl::Param::TapLevels)));
		at = { s.xAt(unit * (0.25 + 0.5 * pattern.position)),
			   s.midY() - (levels.position - 0.5f) * (height() * 0.5f - 12.0f) };
		break;
	}
	case Handle::None: return false;
	}
	if (out != nullptr) {
		*out = at;
	}
	return true;
}

const char* DelayPlane::tooltipAt(visage::Point local) const {
	switch (handleNear(local)) {
	case Handle::Time: return tooltipOf(&control_(dl::Param::Time));
	case Handle::Spread: return tooltipOf(&control_(dl::Param::Spread));
	case Handle::Feedback: return tooltipOf(&control_(dl::Param::Feedback));
	case Handle::Tone: return tooltipOf(&control_(dl::Param::Tone));
	case Handle::Taps: return tooltipOf(&control_(dl::Param::Taps));
	case Handle::Cluster: return tooltipOf(&control_(dl::Param::TapPattern));
	case Handle::None: break;
	}
	return nullptr;
}

DelayPlane::Handle DelayPlane::handleNear(visage::Point at) const {
	Handle best = Handle::None;
	float bestD = kGrab;
	for (const Handle h : { Handle::Time, Handle::Spread, Handle::Feedback, Handle::Tone, Handle::Taps, Handle::Cluster }) {
		visage::Point p {};
		if (handleAt(h, &p)) {
			const float d = std::hypot(p.x - at.x, p.y - at.y);
			if (d <= bestD) {
				bestD = d;
				best = h;
			}
		}
	}
	return best;
}

void DelayPlane::press(visage::Point at) {
	const Handle h = handleNear(at);
	if (h == Handle::None) {
		return;
	}
	dl::TapPicture p;
	if (!delayPicture(host_, id_, &p)) {
		return;
	}
	drag_.strip = strip(); // BEFORE `handle` is set, so it is the live axis — then held
	drag_.handle = h;
	drag_.start = at;
	drag_.firstGain = std::max(p.wetGain, 1.0e-6f);
	drag_.lanePass = p.lanePass > 1.0e-6f ? p.lanePass : 1.0f;
	drag_.unit = handLeftTap(p);
	const dl::Param param = h == Handle::Time       ? dl::Param::Time
	                        : h == Handle::Spread   ? dl::Param::Spread
	                        : h == Handle::Feedback ? dl::Param::Feedback
	                        : h == Handle::Tone     ? dl::Param::Tone
	                        : h == Handle::Taps     ? dl::Param::Taps
	                                                : dl::Param::TapPattern;
	drag_.startValue = fieldOf(param);
	control_(param).beginGesture();
	if (h == Handle::Cluster) {
		control_(dl::Param::TapLevels).beginGesture();
	}
	redraw();
}

void DelayPlane::move(visage::Point at) {
	if (drag_.handle == Handle::None) {
		return;
	}
	const DelayStrip& s = drag_.strip;
	const auto setTo = [&](dl::Param p, double v) {
		const device::ParamDesc& d = dl::kParamDescs[static_cast<size_t>(p)];
		control_(p).setValue(std::clamp(v, d.min, d.max));
	};
	switch (drag_.handle) {
	case Handle::Time: {
		const double sec = std::max(1.0e-6, s.secondsAt(at.x));
		const double base = drag_.unit / std::exp2(drag_.startValue);
		setTo(dl::Param::Time, std::log2(sec / std::max(1.0e-9, base)));
		break;
	}
	case Handle::Spread: {
		const double sec = s.secondsAt(at.x);
		setTo(dl::Param::Spread, (sec / std::max(1.0e-9, drag_.unit) - 1.0) / dl::kSpreadFraction);
		break;
	}
	case Handle::Feedback: {
		// THE TIP'S HEIGHT BACK TO A GAIN on the strip's dB scale, then to the knob: the second repeat is the first
		// times Feedback times what the lane does to a pass.
		const float reach = std::max(1.0f, s.bounds.height() * 0.5f - 4.0f);
		const float h = std::clamp((s.midY() - at.y) / reach, 0.0f, 1.0f);
		const double gain = h <= 0.0f ? 0.0 : std::pow(10.0, (static_cast<double>(h) * -dl::kStripFloorDb + dl::kStripFloorDb) / 20.0);
		setTo(dl::Param::Feedback, gain / drag_.firstGain / drag_.lanePass);
		break;
	}
	case Handle::Tone:
		setTo(dl::Param::Tone, drag_.startValue + static_cast<double>(drag_.start.y - at.y) / kTonePixels);
		break;
	case Handle::Taps:
		setTo(dl::Param::Taps, drag_.startValue + std::round(static_cast<double>(drag_.start.x - at.x) / kTapsPixels));
		break;
	case Handle::Cluster: {
		// EACH COORDINATE KEEPS ITS LAP AND ITS ZONE; THE PUCK MOVES ITS POSITION IN THE ZONE.
		const auto within = [](double coord, double position) {
			const double lap = std::floor(coord);
			const dl::TapZone z = dl::decodeTapCoord(static_cast<float>(coord));
			const double pos = std::clamp(position, 0.0, 0.999);
			return lap + (static_cast<double>(z.zone) + pos) / static_cast<double>(dl::kTapZones);
		};
		const double x = s.secondsAt(at.x) / std::max(1.0e-9, drag_.unit);
		setTo(dl::Param::TapPattern, within(fieldOf(dl::Param::TapPattern), (x - 0.25) / 0.5));
		const float span = std::max(1.0f, s.bounds.height() * 0.5f - 12.0f);
		setTo(dl::Param::TapLevels, within(fieldOf(dl::Param::TapLevels), 0.5 + (s.midY() - at.y) / span));
		break;
	}
	case Handle::None: break;
	}
	redraw();
}

void DelayPlane::release() {
	if (drag_.handle == Handle::None) {
		return;
	}
	const Handle h = drag_.handle;
	drag_.handle = Handle::None;
	const dl::Param param = h == Handle::Time       ? dl::Param::Time
	                        : h == Handle::Spread   ? dl::Param::Spread
	                        : h == Handle::Feedback ? dl::Param::Feedback
	                        : h == Handle::Tone     ? dl::Param::Tone
	                        : h == Handle::Taps     ? dl::Param::Taps
	                                                : dl::Param::TapPattern;
	control_(param).endGesture();
	if (h == Handle::Cluster) {
		control_(dl::Param::TapLevels).endGesture();
	}
	redraw();
}

void DelayPlane::dragForTest(visage::Point from, visage::Point to) {
	press(from);
	move(to);
	release();
}

double DelayPlane::decaySlopeDbForTest() const {
	dl::TapPicture p;
	if (!delayPicture(host_, id_, &p) || !(p.feedback > 0.0f)) {
		return -std::numeric_limits<double>::infinity();
	}
	return 20.0 * std::log10(static_cast<double>(p.feedback));
}

float DelayPlane::floorRightForTest() const {
	dl::TapPicture p;
	if (!delayPicture(host_, id_, &p) || !(p.tapFloor > 0.0)) {
		return 0.0f;
	}
	return strip().xAt(p.tapFloor);
}

void DelayPlane::draw(visage::Canvas& canvas) {
	dl::TapPicture p;
	const bool has = delayPicture(host_, id_, &p);
	const DelayStrip s = strip();
	const double unit = has ? delayHandUnitSeconds(host_, id_, p) : 0.0;
	paintDelayStrip(canvas, s, has ? &p : nullptr, unit);
	if (!has) {
		return;
	}
	// THE FLOOR THE LANE PUTS UNDER TIME, shaded: a tap cannot sit in it (rule 5).
	if (p.tapFloor > 0.0) {
		const float right = s.xAt(p.tapFloor);
		canvas.setColor(withAlpha(kLine, 0.35f));
		canvas.fill(s.bounds.x() + DelayStrip::kInsetLeft, s.bounds.y() + 2.0f,
		            std::max(1.0f, right - s.bounds.x() - DelayStrip::kInsetLeft), s.bounds.height() - 4.0f);
	}
	// THE DECAY LINE through the left repeats' tips: a straight line on the dB scale, its slope the published loop gain.
	const double left = handLeftTap(p);
	if (left > 0.0 && p.feedback > 0.0f) {
		canvas.setColor(withAlpha(kValue, 0.45f));
		float g = std::max(p.wetGain, dl::kStripGainFloor * 1.01f);
		for (int k = 1; k < dl::kStripMaxPasses && left * (k + 1) <= s.span; ++k) {
			const float next = g * p.feedback;
			if (!(next > dl::kStripGainFloor)) {
				break;
			}
			canvas.segment(s.xAt(left * k), s.tipY(0, g), s.xAt(left * (k + 1)), s.tipY(0, next), 1.0f, true);
			g = next;
		}
	}
	// THE LIMITER, while it limits.
	if (p.loopLimit < 0.999f && p.loopLimit > 0.0f) {
		char text[32];
		std::snprintf(text, sizeof(text), "LOOP LIMIT %.1f dB", 20.0 * std::log10(static_cast<double>(p.loopLimit)));
		canvas.setColor(kValue);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kLeft, s.bounds.x() + DelayStrip::kInsetLeft + 4.0f,
		            s.bounds.y() + 1.0f, 160.0f, 11.0f);
	}
	// THE HANDLES.
	const auto mark = [&](Handle h, float r) {
		visage::Point at {};
		if (!handleAt(h, &at)) {
			return;
		}
		const bool held = drag_.handle == h;
		canvas.setColor(held ? kInk : kValue);
		canvas.circle(at.x - r, at.y - r, 2.0f * r);
		canvas.setColor(kSurfaceRaised);
		canvas.circle(at.x - r + 2.0f, at.y - r + 2.0f, 2.0f * (r - 2.0f));
	};
	mark(Handle::Time, 5.0f);
	mark(Handle::Spread, 4.5f);
	mark(Handle::Feedback, 4.5f);
	mark(Handle::Tone, 5.0f);
	mark(Handle::Taps, 4.0f);
	mark(Handle::Cluster, 6.0f);
	// THE HANDLES' NAMES, small, beside the two that are not on a tick.
	canvas.setColor(kMuted);
	visage::Point tone {};
	if (handleAt(Handle::Tone, &tone)) {
		canvas.text("tone", font(kKnobLabelSize), visage::Font::kRight, tone.x - 60.0f, tone.y - 5.0f, 52.0f, 10.0f);
	}
}

bool DelayPlane::update() {
	dl::TapPicture p;
	const bool has = delayPicture(host_, id_, &p);
	double span = 0.0;
	std::vector<dl::StripTick> ticks;
	std::vector<dl::StripGrain> grains;
	std::array<float, 4> extras {};
	if (has) {
		span = strip().span;
		ticks = dl::stripTicks(p, span);
		grains = dl::stripGrains(p);
		extras = { static_cast<float>(p.tapFloor), p.loopLimit, static_cast<float>(fieldOf(dl::Param::Tone)),
			       static_cast<float>(fieldOf(dl::Param::TapLevels)) };
	}
	if (has == drawnHad_ && span == drawnSpan_ && ticks == drawnTicks_ && grains == drawnGrains_ && extras == drawnExtras_) {
		return false;
	}
	drawnHad_ = has;
	drawnSpan_ = span;
	drawnTicks_ = std::move(ticks);
	drawnGrains_ = std::move(grains);
	drawnExtras_ = extras;
	redraw();
	return true;
}

//--------------------------------------------------------------------------------------------------//
// CLEAR                                                                                              //
//--------------------------------------------------------------------------------------------------//

ClearButton::ClearButton(DeviceControl& control) : control_(control) {
	onMouseDown() = [this](const visage::MouseEvent&) { pressForTest(); };
	onMouseUp() = [this](const visage::MouseEvent&) { releaseForTest(); };
	installTooltipHover(*this, *this); // LAST (tooltip.h)
}

void ClearButton::pressForTest() {
	if (held_) {
		return;
	}
	held_ = true;
	control_.beginGesture();
	control_.setValue(1.0);
	redraw();
}

void ClearButton::releaseForTest() {
	if (!held_) {
		return;
	}
	held_ = false;
	control_.setValue(0.0);
	control_.endGesture();
	redraw();
}

const char* ClearButton::tooltipAt(visage::Point) const { return tooltipOf(&control_); }

void ClearButton::draw(visage::Canvas& canvas) {
	canvas.setColor(held_ ? kValue : kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 3.0f);
	canvas.setColor(held_ ? kSurface : kInk);
	canvas.text("Clear", font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
}


//--------------------------------------------------------------------------------------------------//
// THE WINDOW                                                                                         //
//--------------------------------------------------------------------------------------------------//

DelayExpanded::DelayExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                             Notifier* notifier)
    : host_(host), id_(id), notifier_(notifier), chainPin_(host) {
	controls_.resize(static_cast<size_t>(dl::Param::kCount));
	// THE PRESETS, as Resonate's panel builds them (S5). The bank is read now so the first draw lists it; selecting nothing
	// on open means opening the window changes no sound.
	presets_ = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                           std::vector<PresetData> { PresetData { initDevicePreset(dl::kDescriptor) } },
	                                           devicePresetBank(dl::kDescriptor), PresetBrowser::kPanelStyle);
	presets_->setBounds(0.0f, 0.0f, presets_->barWidth(), PresetBrowser::kPanelStyle.height);
	presets_->loadBankFromDisk();
	presetProvider_ = std::make_unique<DevicePresetProvider>(host, dl::kDescriptor, id, std::span<const uint8_t> {}, notifier);
	presets_->setProvider(presetProvider_.get());
	presets_->setNotifier(notifier);
	presets_->onSelected([this](int) { redrawAll(); });
	// THE PANEL OPENS ON THE PRESET THIS DEVICE CAME FROM, and names every later one into the patch (backlog QC).
	bindPresetIdentity(
	    *presets_, [&host, id] { return host.devicePreset(id); },
	    [&host, id](const PresetIdentity& identity) { host.setDevicePreset(id, identity); });
	plane_ = std::make_unique<DelayPlane>(host, id, [this](dl::Param p) -> DeviceControl& { return control(p); });
	addChild(plane_.get());

	const auto box = [&](dl::Param p) {
		auto b = std::make_unique<ValueBox>(control(p), captionOf(p));
		DeviceControl* adapter = &control(p);
		b->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
		addChild(b.get());
		widgets_[static_cast<size_t>(p)] = b.get();
		boxes_.push_back(std::move(b));
	};
	const auto zone = [&](dl::Param p) {
		const device::ParamDesc& d = dl::kParamDescs[static_cast<size_t>(p)];
		auto cell = std::make_unique<ZoneCoordCell>(control(p), captionOf(p), GenericCard::zoneNamesFor(d),
		                                            ZoneSelect::kCoordinatePerLap);
		cell->setFaceDiameter(Knob::kEngineRailDiameter);
		cell->setRowLayout(true);
		DeviceControl* adapter = &control(p);
		cell->box().setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
		addChild(cell.get());
		widgets_[static_cast<size_t>(p)] = cell.get();
		zones_.push_back(std::move(cell));
	};

	// THE TOP ROW: the card's four you reach for — Sync, Time, Mode (its six on the face), Mix — and Dub stacked on Mode.
	box(dl::Param::Sync);
	box(dl::Param::Time);
	mode_ = std::make_unique<SegmentedControl>(control(dl::Param::Mode), registry, dl::kModeNames, notifier_);
	addChild(mode_.get());
	widgets_[static_cast<size_t>(dl::Param::Mode)] = mode_.get();
	box(dl::Param::Mix);
	dub_ = std::make_unique<SwitchChip>(control(dl::Param::Dub), captionOf(dl::Param::Dub));
	addChild(dub_.get());
	widgets_[static_cast<size_t>(dl::Param::Dub)] = dub_.get();
	// THE LOOP.
	box(dl::Param::Feedback);
	box(dl::Param::Tone);
	pingPong_ = std::make_unique<SegmentedControl>(control(dl::Param::PingPong), registry, dl::kPingPongNames, notifier_);
	addChild(pingPong_.get());
	widgets_[static_cast<size_t>(dl::Param::PingPong)] = pingPong_.get();
	box(dl::Param::Spread);
	// THE MODE BAND: every group built, only the live mode's shown.
	for (const dl::Param p : kGrainFour) {
		box(p);
	}
	for (const dl::Param p : kTapeFour) {
		box(p);
	}
	box(dl::Param::Taps);
	zone(dl::Param::TapPattern);
	zone(dl::Param::TapLevels);
	box(dl::Param::TapWidth);
	box(dl::Param::Swarm);
	for (const dl::Param p : kBbdFour) {
		box(p);
	}
	// THE HOLD BAND (S4).
	box(dl::Param::Duck);
	box(dl::Param::DuckRelease);
	freeze_ = std::make_unique<SwitchChip>(control(dl::Param::Freeze), captionOf(dl::Param::Freeze));
	addChild(freeze_.get());
	widgets_[static_cast<size_t>(dl::Param::Freeze)] = freeze_.get();
	clear_ = std::make_unique<ClearButton>(control(dl::Param::Clear));
	addChild(clear_.get());
	widgets_[static_cast<size_t>(dl::Param::Clear)] = clear_.get();

	// THE FEEDBACK FX RAIL — over the lane when there is one, a phantom while there is not (`followLane`). OPEN.
	lane_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kLaneLabel, kLaneLine, &laneOf }, registry, notifier_);
	addChild(lane_.get());
	lane_->onRailWidthChanged([this]() {
		resized();
		redraw();
		minimumSizeChanged();
	});
	void* const held = host.containerSurfaceOf(id);
	(void)followLane(held);
	chainPin_.hold(held);
	(void)lane_->setCollapsed(false);
	(void)applyGates();
}

DelayExpanded::~DelayExpanded() = default;

DeviceControl& DelayExpanded::control(dl::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, id_, dl::kDescriptor, i);
	}
	return *controls_[i];
}

bool DelayExpanded::followLane(void* held) {
	bool moved = lane_->follow(held);
	// A PHANTOM EXACTLY WHILE THERE IS NO LANE: its (+) builds one and opens its add menu once its column exists.
	const bool wantPhantom = laneOf(held) == nullptr;
	if (wantPhantom != phantomShown_) {
		phantomShown_ = wantPhantom;
		if (wantPhantom) {
			lane_->setPhantom(kLanePasses, [this] {
				if (dl::ensureFeedbackLane(host_.containerSurfaceOf(id_)) != nullptr) {
					pendingAddMenu_ = true;
				}
			});
		}
		else {
			lane_->setPhantom({}, {});
		}
		moved = true;
	}
	return moved;
}

int DelayExpanded::modeNow() const {
	return static_cast<int>(std::lround(host_.fieldAt(id_, static_cast<size_t>(dl::Param::Mode))));
}

bool DelayExpanded::gateOpen(dl::Param p) const {
	const device::ParamDesc& d = dl::kParamDescs[static_cast<size_t>(p)];
	return d.gateControl < 0 || device::relevantFor(d, host_.fieldAt(id_, static_cast<size_t>(d.gateControl)));
}

visage::Point DelayExpanded::minimumSize() const {
	const float rows = ValueBox::kHeight + kSwitchHeight + 3.0f * bandRowHeight() + 5.0f * kRowGap;
	visage::Point p { kMinFaceWidth + 2.0f * kPad + lane_->railWidth(), 2.0f * kPad + rows + kMinPlotHeight };
	// AN OPEN RAIL IS A FLOOR ON THE HEIGHT (SKLITTER's rule).
	if (!lane_->collapsed()) {
		p.y = std::max(p.y, lane_->openMinimumHeight());
	}
	return p;
}

void DelayExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<dl::Param>(i)).setModulation(mod, history);
	}
	lane_->setOuterModulation(mod, history, [this] { return watchedDevice(); });
}

void DelayExpanded::setPresentedOn(SurfaceLayers* host) { lane_->setPresentedOn(host); }

void DelayExpanded::onInnerNestChanged() { lane_->setNest(innerNest()); }

void DelayExpanded::resized() { layOut(); }

void DelayExpanded::layOut() {
	laidOutMode_ = modeNow();
	const float rail = lane_->railWidth();
	lane_->setBounds(width() - rail, 0.0f, rail, height());
	const float left = kPad;
	const float colW = std::max(0.0f, width() - rail - 2.0f * kPad);
	const float band = bandRowHeight();
	// EVERY `setBounds` UNCONDITIONAL (CLAUDE.md's DPI rule).
	float y = kPad;
	const float boxW = 78.0f;
	const auto at = [&](dl::Param p) { return widgets_[static_cast<size_t>(p)]; };
	at(dl::Param::Sync)->setBounds(left, y, boxW, ValueBox::kHeight);
	at(dl::Param::Time)->setBounds(left + boxW + kCellGap, y, boxW, ValueBox::kHeight);
	const float mixX = left + colW - kDubWidth - kCellGap - boxW;
	at(dl::Param::Mix)->setBounds(mixX, y, boxW, ValueBox::kHeight);
	dub_->setBounds(left + colW - kDubWidth, y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f, kDubWidth,
	                SwitchChip::kHeight);
	y += ValueBox::kHeight + kRowGap;
	// MODE ON A LINE OF ITS OWN, ACROSS THE FACE: six names on the face need the room (photographed cramped, first draft).
	mode_->setBounds(left, y, colW, kSwitchHeight);
	y += kSwitchHeight + kRowGap;

	const float plotH = std::max(0.0f, height() - y - kPad - 3.0f * band - 3.0f * kRowGap);
	plane_->setBounds(left, y, colW, plotH);
	y += plotH + kRowGap;

	// A BAND: its heading in the margin, then four equal cells.
	const float cellsX = left + kBandLabelWidth;
	const float cell = std::max(0.0f, (colW - kBandLabelWidth - 3.0f * kCellGap) / 4.0f);
	const auto cellX = [&](size_t c) { return cellsX + static_cast<float>(c) * (cell + kCellGap); };
	const float boxTop = (band - ValueBox::kHeight) * 0.5f;
	at(dl::Param::Feedback)->setBounds(cellX(0), y + boxTop, cell, ValueBox::kHeight);
	at(dl::Param::Tone)->setBounds(cellX(1), y + boxTop, cell, ValueBox::kHeight);
	pingPong_->setBounds(cellX(2), y + (band - kSwitchHeight) * 0.5f, cell, kSwitchHeight);
	at(dl::Param::Spread)->setBounds(cellX(3), y + boxTop, cell, ValueBox::kHeight);
	y += band + kRowGap;
	for (const auto* four : { kGrainFour, kTapeFour, kTapsFour, kBbdFour }) {
		for (size_t c = 0; c < 4; ++c) {
			visage::Frame* w = at(four[c]);
			const bool zoned = four[c] == dl::Param::TapPattern || four[c] == dl::Param::TapLevels;
			w->setBounds(cellX(c), zoned ? y : y + boxTop, cell, zoned ? band : ValueBox::kHeight);
		}
	}
	// SWARM: a fifth cell on the grain band — the four narrowed to five across while Granular is live, which is the only
	// time any of them shows.
	{
		const float five = std::max(0.0f, (colW - kBandLabelWidth - 4.0f * kCellGap) / 5.0f);
		for (size_t c = 0; c < 4; ++c) {
			at(kGrainFour[c])->setBounds(cellsX + static_cast<float>(c) * (five + kCellGap), y + boxTop, five, ValueBox::kHeight);
		}
		at(dl::Param::Swarm)->setBounds(cellsX + 4.0f * (five + kCellGap), y + boxTop, five, ValueBox::kHeight);
	}
	// THE HOLD BAND (S4): Duck and its Release, the Freeze, the Clear.
	y += band + kRowGap;
	at(dl::Param::Duck)->setBounds(cellX(0), y + boxTop, cell, ValueBox::kHeight);
	at(dl::Param::DuckRelease)->setBounds(cellX(1), y + boxTop, cell, ValueBox::kHeight);
	freeze_->setBounds(cellX(2), y + (band - SwitchChip::kHeight) * 0.5f, std::min(cell, 64.0f), SwitchChip::kHeight);
	clear_->setBounds(cellX(3), y + (band - SwitchChip::kHeight) * 0.5f, std::min(cell, 64.0f), SwitchChip::kHeight);
	(void)applyGates();
}

bool DelayExpanded::applyGates() {
	bool changed = false;
	for (size_t i = 0; i < widgets_.size(); ++i) {
		visage::Frame* w = widgets_[i];
		if (w == nullptr) {
			continue;
		}
		// THE CARD'S RULE: a control its gate rules out is not drawn (engine settings hidden, not greyed).
		const bool on = gateOpen(static_cast<dl::Param>(i));
		if (w->isVisible() != on) {
			w->setVisible(on);
			changed = true;
		}
	}
	return changed;
}

bool DelayExpanded::updateContent() {
	bool moved = false;
	if (modeNow() != laidOutMode_) {
		layOut();
		moved = true;
	}
	if (applyGates()) {
		moved = true;
	}
	if (moved) {
		redraw();
	}
	if (plane_->update()) {
		moved = true;
	}
	// THE LANE, RE-ASKED EVERY TICK: the handle is contemporaneous (trap T9).
	void* const held = host_.containerSurfaceOf(id_);
	moved = followLane(held) || moved;
	moved = lane_->updateCards() > 0 || moved;
	chainPin_.hold(held);
	if (pendingAddMenu_ && lane_->column() != nullptr) {
		pendingAddMenu_ = false;
		lane_->column()->showAddMenu();
		moved = true;
	}
	return moved;
}

void DelayExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width() - lane_->railWidth(), height());
	// THE BANDS' HEADINGS, in the margin: LOOP, and the live mode's own.
	const float band = bandRowHeight();
	const float loopY = plane_->bottom() + kRowGap;
	const float modeY = loopY + band + kRowGap;
	canvas.setColor(kMuted);
	canvas.text("LOOP", font(kKnobLabelSize), visage::Font::kLeft, kPad, loopY, kBandLabelWidth, band);
	const char* heading = nullptr;
	switch (static_cast<dl::Mode>(std::clamp(modeNow(), 0, static_cast<int>(dl::Mode::kCount) - 1))) {
	case dl::Mode::Granular: heading = "GRAIN"; break;
	case dl::Mode::Tape: heading = "TAPE"; break;
	case dl::Mode::MultiTap: heading = "TAPS"; break;
	case dl::Mode::Bbd: heading = "BUCKETS"; break;
	default: break;
	}
	if (heading != nullptr) {
		canvas.text(heading, font(kKnobLabelSize), visage::Font::kLeft, kPad, modeY, kBandLabelWidth, band);
	}
	canvas.setColor(kMuted);
	canvas.text("HOLD", font(kKnobLabelSize), visage::Font::kLeft, kPad, modeY + band + kRowGap, kBandLabelWidth, band);
}

std::vector<dl::Param> DelayExpanded::addressedParamsForTest() const {
	std::vector<dl::Param> out;
	std::vector<ControlFace::Offered> offered;
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ControlFace*>(frame)) {
			offered.clear();
			face->offeredControls(offered);
			for (const ControlFace::Offered& o : offered) {
				for (size_t i = 0; i < controls_.size(); ++i) {
					const auto p = static_cast<dl::Param>(i);
					if (o.road == ControlFace::Road::Face && controls_[i] != nullptr
					    && static_cast<const device::Modulatable*>(controls_[i].get()) == o.control
					    && std::find(out.begin(), out.end(), p) == out.end()) {
						out.push_back(p);
					}
				}
			}
		}
		for (const visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(this);
	return out;
}

} // namespace feathers::ui
