/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/drum_hit_picture.h"

#include "plugin/ui/celestial.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/type_in.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {

namespace {
uint32_t alphaOf(uint32_t colour, float alpha) {
	const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
	return (colour & 0x00ffffffu) | (a << 24);
}
} // namespace

//------------//
// HitPicture //
//------------//

namespace {

/// The decay as the eye reads it: the cube root of the level, so −60 dB is a tenth of full and a tail stays legible.
float seen(double level) { return static_cast<float>(std::cbrt(std::clamp(level, 0.0, 1.0))); }

constexpr double kLowHz = 20.0;
constexpr double kHighHz = 20000.0;
constexpr float kHandleRadius = 6.0f;
constexpr float kHitSlop = 11.0f;
/// Two handles' marks stand at least this far apart (see HitPicture::marks).
constexpr float kMarkGap = 9.0f;
/// The Drive handle's floor: at Drive 0 it stands this share of the envelope up, clear of the Decay handle's line by more
/// than the hit slop.
constexpr float kDriveFloor = 0.25f;
/// The Hold handle stands this far under the level line, clear of the Level handle by more than the hit slop.
constexpr float kHoldDrop = 16.0f;

} // namespace

const char* HitPicture::tooltipAt(visage::Point local) const {
	const Handle h = handleUnder(local);
	if (h != Handle::kCount) {
		if (const device::Modulatable* c = controlOf(h); c != nullptr && c->desc().tooltip != nullptr) {
			return c->desc().tooltip;
		}
	}
	return "The hit as it plays: frequencies over time above, the loudness below. Drag a dot to move the control it stands for.";
}

void HitPicture::offeredControls(std::vector<Offered>& out) const {
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (device::Modulatable* c = controlOf(h); c != nullptr) {
			out.push_back({ c, Road::Gesture });
		}
		if (device::Modulatable* second = secondOf(h); second != nullptr) {
			out.push_back({ second, Road::Gesture });
		}
	}
}

HitPicture::HitPicture() {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		fine_ = e.isShiftDown();
		if (modHandles_) {
			const Handle h = handleUnder(e.position);
			if (h != Handle::kCount && controlOf(h) != nullptr) {
				ModHandle& mod = mods_[static_cast<size_t>(h)];
				// THE SOURCES MENU on a secondary press, and a ROUTE GESTURE when a source is armed (or Alt is held over the stroke): the knob's roads.
				if (e.shouldTriggerPopup() && mod.showMenu(*this)) {
					return;
				}
				const visage::Point c = markAt(h);
				const size_t axis = mod.routeAxisFor(e.isAltDown(), e.position, c.x, c.y);
				if (axis != ModHandle::npos) {
					routeHandle_ = h;
					mod.beginRoute(axis, e);
					redraw();
					return;
				}
			}
		}
		press(e.position);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (routeHandle_ != Handle::kCount) {
			mods_[static_cast<size_t>(routeHandle_)].dragRoute(e);
			redraw();
			return;
		}
		fine_ = e.isShiftDown();
		drag(e.position);
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routeHandle_ != Handle::kCount) {
			(void)mods_[static_cast<size_t>(routeHandle_)].endRoute();
			routeHandle_ = Handle::kCount;
			redraw();
			return;
		}
		release();
	};
	installTooltipHover(*this, *this); // LAST, after every `=` above
}

std::vector<ModHandleAxis> HitPicture::modAxesFor(Handle h) {
	// EACH AXIS: the control it edits, whether it runs across (time) or up and down, and the plot's own map from the control's VALUE to a
	// coordinate (the same arithmetic `handleAt` and `drag` use, restated per control). Read at the live laws, so an arc follows the picture.
	const auto modulated = [](const device::Modulatable* c) { return c->display().modulated; };
	const auto across = [this](device::Modulatable* c, std::function<float(double)> f) { return ModHandleAxis { c, true, std::move(f) }; };
	const auto upDown = [this](device::Modulatable* c, std::function<float(double)> f) { return ModHandleAxis { c, false, std::move(f) }; };
	const auto xOfSeconds = [this](double s) { return xOf(s); };
	const auto xOfLog2 = [this](double v) { return xOf(std::exp2(v)); };
	const auto yOfLog2Hz = [this](double v) { return yOfHz(std::exp2(v)); };
	const Controls& c = controls_;
	std::vector<ModHandleAxis> out;
	switch (h) {
	case Handle::Tune:
		out.push_back(upDown(c.tune, [this, c, modulated](double v) { return yOfHz(laws_.hz * std::exp2((v - modulated(c.tune)) / 12.0)); }));
		break;
	case Handle::SweepTime: out.push_back(across(c.sweepTime, [xOfLog2](double v) { return xOfLog2(v); })); break;
	case Handle::Decay:
		out.push_back(across(c.decay, [this, xOfLog2](double v) {
			const double mult = decaySeconds_ > 0.0 ? laws_.t60 / decaySeconds_ : 1.0;
			return xOf(std::exp2(v) * mult);
		}));
		break;
	case Handle::Level: out.push_back(upDown(c.level, [this](double v) { return yOfLevel(v); })); break;
	case Handle::Drive:
		out.push_back(upDown(c.drive, [this](double v) {
			return envBottom() - (kDriveFloor + (1.0f - kDriveFloor) * static_cast<float>(v)) * (envBottom() - envTop());
		}));
		if (c.driveTone != nullptr) {
			out.push_back(across(c.driveTone, [this, c, modulated](double v) {
				return plotLeft() + 0.25f * (plotRight() - plotLeft()) + static_cast<float>(v - modulated(c.driveTone)) * 0.5f * (plotRight() - plotLeft());
			}));
		}
		break;
	case Handle::WireDecay:
		out.push_back(across(c.wireDecay, [this, c, modulated](double v) { return xOf(std::exp2(v) * laws_.wireT60 / std::exp2(modulated(c.wireDecay))); }));
		break;
	case Handle::ClickDecay: out.push_back(across(c.clickDecay, [xOfLog2](double v) { return xOfLog2(v); })); break;
	case Handle::BellStart: out.push_back(upDown(c.bellStart, [yOfLog2Hz](double v) { return yOfLog2Hz(v); })); break;
	case Handle::BellEnd:
		out.push_back(upDown(c.bellEnd, [yOfLog2Hz](double v) { return yOfLog2Hz(v); }));
		if (c.bellTime != nullptr) {
			out.push_back(across(c.bellTime, [xOfLog2](double v) { return xOfLog2(v); }));
		}
		break;
	case Handle::Bell2:
		out.push_back(across(c.bell2Time, [xOfLog2](double v) { return xOfLog2(v); }));
		if (c.bell2 != nullptr) {
			out.push_back(upDown(c.bell2, [yOfLog2Hz](double v) { return yOfLog2Hz(v); }));
		}
		break;
	case Handle::Hold: out.push_back(across(c.hold, [xOfSeconds](double v) { return xOfSeconds(v / 1000.0); })); break;
	case Handle::Dip:
		out.push_back(upDown(c.dip, [this](double v) {
			return yOfLevel(levelDb_ + 20.0 * std::log10(std::max(1.0e-3, 1.0 - v)));
		}));
		if (c.dipTime != nullptr) {
			out.push_back(across(c.dipTime, [xOfSeconds](double v) { return xOfSeconds(v / 1000.0); }));
		}
		break;
	case Handle::ClickSweep:
		out.push_back(upDown(c.clickSweep, [this](double v) { return yOfHz(laws_.hz * std::exp2(laws_.sweepOctaves + v)); }));
		if (c.clickSweepTime != nullptr) {
			out.push_back(across(c.clickSweepTime, [xOfLog2](double v) { return xOfLog2(v); }));
		}
		break;
	case Handle::StartPhase:
		out.push_back(upDown(c.startPhase, [this](double v) { return envBottom() - static_cast<float>(v / 90.0) * (envBottom() - envTop()); }));
		break;
	case Handle::Spread: out.push_back(across(c.spread, [xOfLog2](double v) { return xOfLog2(v); })); break;
	case Handle::ClapBand:
		out.push_back(upDown(c.clapHighPass, [yOfLog2Hz](double v) { return yOfLog2Hz(v); }));
		if (c.clapQ != nullptr) {
			out.push_back(across(c.clapQ, [this, c, modulated](double v) {
				return plotLeft() + 0.86f * (plotRight() - plotLeft()) + 80.0f * static_cast<float>(std::log2(std::max(1.0e-3, v / std::max(1.0e-3, modulated(c.clapQ)))));
			}));
		}
		break;
	default: break;
	}
	return out;
}

void HitPicture::rebuildMods() {
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		mods_[i] = controlOf(h) != nullptr ? ModHandle(modAxesFor(h)) : ModHandle {};
	}
}

void HitPicture::show(drum::Kind kind, const drum::Laws& laws, double decaySeconds, double levelDb, const Controls& controls) {
	const bool same = kind == kind_ && laws == laws_ && decaySeconds == decaySeconds_ && levelDb == levelDb_ && controls == controls_;
	if (same) {
		return;
	}
	kind_ = kind;
	laws_ = laws;
	decaySeconds_ = decaySeconds;
	levelDb_ = levelDb;
	controls_ = controls;
	rebuildMods();
	redraw();
}

float HitPicture::xOf(double seconds) const {
	// A SOFT-KNEE LOG TIME AXIS, FIXED: u = ln(1 + t / kAxisKnee) / ln(1 + kAxisMax / kAxisKnee). Below the knee (1 ms) it is
	// linear, so 0 stands exactly on the plot's left edge and a handle leaves it smoothly; above it, it is logarithmic, so a
	// pixel is the same FACTOR of time anywhere (about 130 px a decade here). Every time a control can reach is on it, so the
	// axis never moves under a hand.
	const double u = std::log1p(std::max(0.0, seconds) / kAxisKnee) / std::log1p(kAxisMax / kAxisKnee);
	return plotLeft() + static_cast<float>(std::clamp(u, 0.0, 1.0)) * (plotRight() - plotLeft());
}

double HitPicture::secondsAt(float x) const {
	const double u = std::clamp(static_cast<double>((x - plotLeft()) / (plotRight() - plotLeft())), 0.0, 1.0);
	return kAxisKnee * std::expm1(u * std::log1p(kAxisMax / kAxisKnee));
}

float HitPicture::yOfHz(double hz) const {
	const double t = (std::log2(std::clamp(hz, kLowHz, kHighHz)) - std::log2(kLowHz)) / (std::log2(kHighHz) - std::log2(kLowHz));
	return specBottom() - static_cast<float>(t) * (specBottom() - specTop());
}

double HitPicture::hzAt(float y) const {
	const double t = std::clamp(static_cast<double>((specBottom() - y) / (specBottom() - specTop())), 0.0, 1.0);
	return std::exp2(std::log2(kLowHz) + t * (std::log2(kHighHz) - std::log2(kLowHz)));
}

float HitPicture::yOfLevel(double db) const {
	// −60 dB at the floor, +6 dB at the top: the Level control's own range.
	const double t = (std::clamp(db, -60.0, 6.0) + 60.0) / 66.0;
	return envBottom() - static_cast<float>(t) * (envBottom() - envTop());
}

device::Modulatable* HitPicture::controlOf(Handle h) const {
	switch (h) {
	case Handle::Tune: return controls_.tune;
	case Handle::SweepTime: return controls_.sweepTime;
	case Handle::Decay: return controls_.decay;
	case Handle::Level: return controls_.level;
	case Handle::Drive: return controls_.drive;
	case Handle::WireDecay: return controls_.wireDecay;
	case Handle::ClickDecay: return controls_.clickDecay;
	case Handle::BellStart: return controls_.bellStart;
	case Handle::BellEnd: return controls_.bellEnd;
	case Handle::Bell2: return controls_.bell2Time; // its ring; its Hz is the second axis while Free
	case Handle::StartPhase: return controls_.startPhase;
	case Handle::Spread: return controls_.spread;
	case Handle::ClapBand: return controls_.clapHighPass;
	case Handle::Hold: return controls_.hold;
	case Handle::Dip: return controls_.dip;
	case Handle::ClickSweep: return controls_.clickSweep;
	default: return nullptr;
	}
}

bool HitPicture::handleShown(Handle h) const { return controlOf(h) != nullptr; }

visage::Point HitPicture::handleAt(Handle h) const {
	switch (h) {
	case Handle::Tune: return { plotLeft() + 0.72f * (plotRight() - plotLeft()), yOfHz(laws_.hz) }; // on the resting pitch, late
	case Handle::SweepTime: {
		const double t = 3.0 * laws_.sweepTau; // the sweep's time: 95% of the drop done
		return { xOf(t), yOfHz(laws_.hz * std::exp2(laws_.sweepOctaves * std::exp(-3.0))) };
	}
	case Handle::Decay: return { xOf(laws_.t60), envBottom() };
	case Handle::Level: return { plotLeft() + 4.0f, yOfLevel(levelDb_) };
	case Handle::Drive: {
		// THE KNEE: a quarter of the way along, its height Drive over the envelope's upper three quarters, so at Drive 0 it stands a
		// quarter up and never on the floor line the Decay handle walks (audition bug b, 2026-11-06: at Drive 0 the two dots met
		// whenever the T60 sat a quarter of the way along, and a press took whichever was nearer).
		return { plotLeft() + 0.25f * (plotRight() - plotLeft()),
			     envBottom() - (kDriveFloor + (1.0f - kDriveFloor) * static_cast<float>(laws_.drive)) * (envBottom() - envTop()) };
	}
	// A SNARE'S: the wires at their T60 on their band, the click at its T60 high up, the bell at its start and its landing.
	case Handle::WireDecay: return { xOf(laws_.wireT60), yOfHz(drum::wireCentreHz(laws_.tone, laws_.hz)) };
	case Handle::ClickDecay: return { xOf(laws_.clickT60), yOfHz(9000.0) };
	case Handle::BellStart: return { plotLeft() + 6.0f, yOfHz(laws_.bellStartHz) };
	case Handle::BellEnd: return { xOf(3.0 * laws_.bellTau), yOfHz(laws_.bellEndHz) };
	// THE SECOND BELL at its ring's T60, where it rests: Ratio × the first bell's end (Linked) or its own Hz (Free).
	case Handle::Bell2: return { xOf(laws_.bell2T60), yOfHz(laws_.bell2Linked ? laws_.bellEndHz * laws_.bell2Ratio : laws_.bell2Hz) };
	// A KICK'S: Hold at its end on the envelope's top, the fishtail at its deepest, the click sweep at its start on the trace.
	// HOLD only says WHEN (its height means nothing), so it stands a little under the envelope's top, never on the Level
	// handle's line: at Hold 0 the two were 4 px apart and a press took either (the audition's bug b, again).
	case Handle::Hold: return { xOf(laws_.kickHold), yOfLevel(levelDb_) + kHoldDrop };
	case Handle::Dip: {
		const double depthDb = 20.0 * std::log10(std::max(1.0e-3, 1.0 - laws_.kickDip));
		return { xOf(laws_.kickDipTime), yOfLevel(levelDb_ + depthDb) };
	}
	case Handle::ClickSweep: return { xOf(3.0 * laws_.clickSweepTau), yOfHz(laws_.hz * std::exp2(laws_.sweepOctaves + laws_.clickSweepOctaves)) };
	// ONTO THE PICTURE (§14 lever 3): a Kick's Start Phase a little in from the envelope's left, as high as its degrees; a Clap's
	// Spread above its band at the last hand's time; its band's handle at its High Pass corner, late in the plot.
	case Handle::StartPhase:
		return { plotLeft() + 26.0f, envBottom() - static_cast<float>(laws_.startPhase / (std::numbers::pi / 2.0)) * (envBottom() - envTop()) };
	case Handle::Spread:
		return { xOf(laws_.spreadSeconds), yOfHz(drum::clapCentreHz(laws_.tone) * laws_.hz / drum::kindBaseHz(drum::Kind::Clap)) - 22.0f };
	case Handle::ClapBand: return { plotLeft() + 0.86f * (plotRight() - plotLeft()), yOfHz(laws_.clapHighPassHz) };
	default: return {};
	}
}

std::array<visage::Point, HitPicture::kHandleCount> HitPicture::marks() const {
	// WHERE EACH HANDLE IS DRAWN AND TAKEN: its place, nudged up or down in fixed steps (0, +9, -9, +18, -18 ... px, in the enum's
	// order) until it stands at least kMarkGap from every mark placed before it. Handles that coincide in time, or nearly in
	// time and pitch (everything short), then each keep a hit point of their own. A drag still maps from `handleAt`.
	std::array<visage::Point, kHandleCount> out {};
	std::array<bool, kHandleCount> placed {};
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (!handleShown(h)) {
			continue;
		}
		const visage::Point base = handleAt(h);
		visage::Point chosen = base;
		for (int k = 0; k < 13; ++k) {
			const float dy = static_cast<float>((k + 1) / 2) * kMarkGap * ((k & 1) != 0 ? 1.0f : -1.0f);
			const visage::Point at { base.x, base.y + dy };
			if (at.y < kHandleRadius || at.y > height() - kHandleRadius) {
				continue;
			}
			bool clear = true;
			for (size_t j = 0; j < i && clear; ++j) {
				clear = !placed[j] || std::hypot(out[j].x - at.x, out[j].y - at.y) >= kMarkGap;
			}
			if (clear) {
				chosen = at;
				break;
			}
		}
		out[i] = chosen;
		placed[i] = true;
	}
	return out;
}

visage::Point HitPicture::markAt(Handle h) const {
	return handleShown(h) ? marks()[static_cast<size_t>(h)] : visage::Point {};
}

HitPicture::Handle HitPicture::handleUnder(visage::Point p) const {
	Handle held = Handle::kCount;
	float best = kHitSlop;
	const std::array<visage::Point, kHandleCount> marks = this->marks();
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (!handleShown(h)) {
			continue;
		}
		const visage::Point at = marks[i];
		const float d = std::hypot(at.x - p.x, at.y - p.y);
		if (d <= best) {
			best = d;
			held = h;
		}
	}
	return held;
}

device::Modulatable* HitPicture::secondOf(Handle h) const {
	// THE OTHER AXIS OF A TWO-WAY HANDLE: what across (or, on the second bell, up and down) writes.
	switch (h) {
	case Handle::Drive: return controls_.driveTone;
	case Handle::BellEnd: return controls_.bellTime;
	case Handle::Bell2: return laws_.bell2Linked ? nullptr : controls_.bell2; // linked: its ratio is the place's Metal
	case Handle::Dip: return controls_.dipTime;
	case Handle::ClickSweep: return controls_.clickSweepTime;
	case Handle::ClapBand: return controls_.clapQ;
	default: return nullptr;
	}
}

void HitPicture::press(visage::Point p) {
	if (dragging_) {
		release(); // a release that never came: end its gesture before this press begins another
	}
	held_ = handleUnder(p);
	if (held_ == Handle::kCount) {
		return;
	}
	device::Modulatable* c = controlOf(held_);
	if (c == nullptr) {
		held_ = Handle::kCount; // nothing there to hold
		return;
	}
	heldControl_ = c;
	heldSecond_ = secondOf(held_);
	dragging_ = true;
	tuneFine_ = fine_;
	slack_ = 0.0f;
	pressAt_ = p;
	startValue_ = c->value();
	startX_ = handleAt(held_).x; // THE VALUE'S PLACE, not the mark's offset: a drag maps from where the value is
	startY_ = handleAt(held_).y;
	startMultiplier_ = decaySeconds_ > 0.0 ? laws_.t60 / decaySeconds_ : 1.0;
	if (held_ == Handle::WireDecay) {
		startMultiplier_ = laws_.wireT60 / std::exp2(startValue_); // Length's multiplier on the wires
	}
	if (heldSecond_ != nullptr) {
		startSecond_ = heldSecond_->value();
		// A TWO-WAY HANDLE'S GESTURE IS ONE UNDO STEP, not one for each control it writes.
		groupHistory_ = historyOf(*c);
		if (groupHistory_ != nullptr) {
			groupHistory_->beginGroup("Move drum handle");
		}
		heldSecond_->beginGesture();
	}
	c->beginGesture();
}

void HitPicture::drag(visage::Point p) {
	if (!dragging_ || held_ == Handle::kCount) {
		return;
	}
	device::Modulatable* c = heldControl_;
	device::Modulatable* second = heldSecond_;
	if (c == nullptr) {
		return;
	}
	if (held_ == Handle::Tune && fine_ != tuneFine_) {
		// SHIFT CHANGING MID-DRAG RE-ANCHORS (the knob's rule): the drag restarts from where the value is, at the new gearing.
		tuneFine_ = fine_;
		startValue_ = c->value();
		pressAt_.y = p.y;
	}
	const float dx = p.x - pressAt_.x;
	const float dy = p.y - pressAt_.y;
	const float envH = envBottom() - envTop();
	// THE TIME UNDER THE HAND, AS FAR AS THE CONTROL CAN FOLLOW IT: `toValue` maps seconds to the control's own unit, and a hand
	// beyond where that leaves the control's range is taken to be AT the range's end. Without it the value stops at its clamp
	// while the hand goes on (or comes back from beyond it), and a drag back sets nothing until the hand is home again.
	const auto reach = [&](const device::Modulatable* control, auto&& toValue) {
		const double lo = control->desc().min;
		const double hi = control->desc().max;
		float x = startX_ + dx - slack_;
		// THE AXIS'S OWN ENDS first: a hand beyond the plot is at its edge, and the excess is kept like a control's.
		const float inside = std::clamp(x, plotLeft(), plotRight());
		slack_ += x - inside;
		x = inside;
		const double v = toValue(secondsAt(x));
		if (v > hi) {
			float a = plotLeft(), b = x;
			for (int i = 0; i < 40; ++i) {
				const float m = 0.5f * (a + b);
				(toValue(secondsAt(m)) > hi ? b : a) = m;
			}
			slack_ += x - a; // THE HAND'S EXCESS IS KEPT, so coming back moves the value at once rather than after the way home
			x = a;
		}
		else if (v < lo) {
			float a = x, b = plotRight();
			for (int i = 0; i < 40; ++i) {
				const float m = 0.5f * (a + b);
				(toValue(secondsAt(m)) < lo ? a : b) = m;
			}
			slack_ += x - b;
			x = b;
		}
		return secondsAt(x);
	};
	const auto log2s = [](double floor, double mult = 1.0) { return [=](double t) { return std::log2(std::max(floor, t) / mult); }; };
	const auto millis = [](double t) { return 1000.0 * t; };
	switch (held_) {
	case Handle::Tune: {
		// SEMITONES BY THE PLOT'S OWN SCALE: a drag of one octave's height is twelve.
		const float perOctave = (specBottom() - specTop()) / static_cast<float>(std::log2(kHighHz / kLowHz));
		const double raw = startValue_ - 12.0 * static_cast<double>(dy / perOctave);
		// WHOLE SEMITONES, A DRUM'S NOTES; Shift is the fine gearing (`kFineAdjust` of the travel, not rounded).
		c->setValue(fine_ ? startValue_ + (raw - startValue_) * static_cast<double>(kFineAdjust) : std::round(raw));
		break;
	}
	case Handle::SweepTime:
		// THE LANDING WHERE THE HAND IS: Sweep Time = the time under it, in log2 seconds (the control's unit).
		c->setValue(log2s(1.0e-3)(reach(c, log2s(1.0e-3))));
		break;
	case Handle::Decay:
		// THE T60 WHERE THE HAND IS; Decay is that over Length's multiplier (T60 / Decay at the press), in log2 seconds.
		c->setValue(log2s(1.0e-3, startMultiplier_)(reach(c, log2s(1.0e-3, startMultiplier_))));
		break;
	case Handle::Level: c->setValue(startValue_ - 66.0 * static_cast<double>(dy / envH)); break;
	case Handle::Drive:
		// UP AND DOWN, HOW HARD; ACROSS, THE TONE BEFORE IT (§14 lever 3): half the plot's width is the tone's whole travel.
		c->setValue(startValue_ - static_cast<double>(dy / ((1.0f - kDriveFloor) * envH)));
		if (second != nullptr) {
			second->setValue(startSecond_ + static_cast<double>(dx / (0.5f * (plotRight() - plotLeft()))));
		}
		break;
	case Handle::WireDecay: c->setValue(log2s(1.0e-3, startMultiplier_)(reach(c, log2s(1.0e-3, startMultiplier_)))); break;
	case Handle::ClickDecay: c->setValue(log2s(1.0e-4)(reach(c, log2s(1.0e-4)))); break;
	case Handle::BellStart: c->setValue(std::log2(hzAt(startY_ + dy))); break;
	case Handle::BellEnd:
		// UP AND DOWN, WHERE IT LANDS; ACROSS, WHEN (Bell Time: 95% of the glide).
		c->setValue(std::log2(hzAt(startY_ + dy)));
		if (second != nullptr) {
			second->setValue(log2s(1.0e-3)(reach(second, log2s(1.0e-3))));
		}
		break;
	case Handle::Bell2:
		// ACROSS, HOW LONG IT RINGS; UP AND DOWN, WHERE IT RESTS while Free (Linked, the place's Metal sets its ratio).
		c->setValue(log2s(1.0e-3)(reach(c, log2s(1.0e-3))));
		if (second != nullptr) {
			second->setValue(std::log2(hzAt(startY_ + dy)));
		}
		break;
	case Handle::Hold: c->setValue(millis(reach(c, millis))); break; // ms
	case Handle::Dip: {
		// ACROSS, WHEN (ms); UP AND DOWN, HOW DEEP: the depth whose level, below the peak, is where the hand is.
		if (second != nullptr) {
			second->setValue(millis(reach(second, millis)));
		}
		const double db = static_cast<double>((envBottom() - (startY_ + dy)) / envH) * 66.0 - 60.0;
		c->setValue(1.0 - std::pow(10.0, std::min(0.0, db - levelDb_) / 20.0));
		break;
	}
	case Handle::ClickSweep:
		// UP AND DOWN, HOW FAR ABOVE THE SWEEP'S TOP IT STARTS (octaves); ACROSS, ITS TIME.
		c->setValue(std::log2(hzAt(startY_ + dy) / laws_.hz) - laws_.sweepOctaves);
		if (second != nullptr) {
			second->setValue(log2s(1.0e-5)(reach(second, log2s(1.0e-5))));
		}
		break;
	case Handle::StartPhase:
		// UP AND DOWN, DEGREES: the envelope's height is the phase's whole 90.
		c->setValue(startValue_ - 90.0 * static_cast<double>(dy / envH));
		break;
	case Handle::Spread:
		// ACROSS, WHEN THE LAST HAND LANDS (Spread, in log2 seconds).
		c->setValue(log2s(1.0e-3)(reach(c, log2s(1.0e-3))));
		break;
	case Handle::ClapBand:
		// UP AND DOWN, THE HIGH PASS CORNER WHERE THE HAND IS; ACROSS, THE BAND'S Q (an octave of Q per 80 px).
		c->setValue(std::log2(hzAt(startY_ + dy)));
		if (second != nullptr) {
			second->setValue(startSecond_ * std::exp2(static_cast<double>(dx) / 80.0));
		}
		break;
	default: break;
	}
}

void HitPicture::release() {
	// ENDED ON WHAT THE PRESS BEGAN, not on what `controls_` names now.
	if (dragging_ && held_ != Handle::kCount && heldControl_ != nullptr) {
		heldControl_->endGesture();
		if (heldSecond_ != nullptr) {
			heldSecond_->endGesture();
		}
	}
	if (groupHistory_ != nullptr) {
		groupHistory_->endGroup();
		groupHistory_ = nullptr;
	}
	dragging_ = false;
	held_ = Handle::kCount;
	heldControl_ = nullptr;
	heldSecond_ = nullptr;
}

void HitPicture::dragForTest(Handle h, float dx, float dy, bool fine) {
	const visage::Point at = markAt(h);
	fine_ = fine;
	press(at);
	drag({ at.x + dx, at.y + dy });
	release();
	fine_ = false;
}

void HitPicture::draw(visage::Canvas& canvas) {
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 6.0f);
	const float w = plotRight() - plotLeft();
	// THE GRID: 100 Hz, 1 kHz, 10 kHz across the top; the floor of the envelope; faint, labelled.
	canvas.setColor(alphaOf(kLine, 0.6f));
	for (const double hz : { 100.0, 1000.0, 10000.0 }) {
		canvas.fill(plotLeft(), yOfHz(hz), w, 1.0f);
	}
	canvas.fill(plotLeft(), envBottom(), w, 1.0f);
	canvas.setColor(kMuted);
	canvas.text("100", font(kKnobLabelSize), visage::Font::kLeft, plotRight() - 26.0f, yOfHz(100.0) - 11.0f, 26.0f, 10.0f);
	canvas.text("1k", font(kKnobLabelSize), visage::Font::kLeft, plotRight() - 26.0f, yOfHz(1000.0) - 11.0f, 26.0f, 10.0f);
	canvas.text("10k", font(kKnobLabelSize), visage::Font::kLeft, plotRight() - 26.0f, yOfHz(10000.0) - 11.0f, 26.0f, 10.0f);

	constexpr int kSteps = 240; // about three pixels a step: the log axis spends its first pixels on the first microseconds
	// A GATED COMPONENT (a Snare's Shape) falls in 15 ms past its gate, in the picture as in the hit.
	const auto gatedAt = [](double t, double gate) { return gate > 0.0 && t > gate ? std::pow(10.0, -3.0 * (t - gate) / 0.015) : 1.0; };
	const double bodyGate = kind_ == drum::Kind::Snare ? laws_.bodyGate : 0.0;
	const auto line = [&](double hz, double t60, double level, float thick) {
		const float y = yOfHz(hz);
		for (int i = 0; i < kSteps; ++i) {
			const double t = secondsAt(plotLeft() + w * static_cast<float>(i) / kSteps);
			canvas.setColor(alphaOf(kValue, seen(level * std::pow(10.0, -3.0 * t / std::max(1.0e-3, t60)) * gatedAt(t, bodyGate))));
			canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, y - 0.5f * thick, w / kSteps + 0.5f, thick);
		}
	};
	const auto band = [&](double centre, double t60, double level) {
		const float y0 = yOfHz(centre * 1.6);
		const float y1 = yOfHz(centre / 1.6);
		for (int i = 0; i < kSteps; ++i) {
			const double t = secondsAt(plotLeft() + w * static_cast<float>(i) / kSteps);
			canvas.setColor(alphaOf(kAccent, 0.55f * seen(level * std::pow(10.0, -3.0 * t / std::max(1.0e-3, t60)))));
			canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, y0, w / kSteps + 0.5f, y1 - y0);
		}
	};
	switch (kind_) {
	case drum::Kind::Kick: {
		// THE SWEEP: the body's pitch falling to rest, a bold trace whose brightness is its level.
		for (int i = 0; i < kSteps; ++i) {
			const double t = secondsAt(plotLeft() + w * static_cast<float>(i) / kSteps);
			const double hz = laws_.hz * std::exp2(laws_.sweepOctaves * std::exp(-t / laws_.sweepTau)
			                                       + laws_.clickSweepOctaves * std::exp(-t / laws_.clickSweepTau));
			canvas.setColor(alphaOf(kValue, seen(std::pow(10.0, -3.0 * t / std::max(1.0e-3, laws_.t60)))));
			canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, yOfHz(hz) - 2.0f, w / kSteps + 0.5f, 4.0f);
		}
		band(4000.0, 0.008 + 0.024 * (1.0 - laws_.snap), laws_.snap);
		break;
	}
	case drum::Kind::Membrane:
	case drum::Kind::Snare: {
		const int modes = kind_ == drum::Kind::Membrane ? drum::kMembraneModes : drum::kSnareHeadModes;
		for (int k = 0; k < modes; ++k) {
			const double ratio = drum::modeRatio(k, laws_.metal);
			line(laws_.hz * ratio, drum::modeT60(laws_.t60, ratio, laws_.metal), std::pow(ratio, -(1.1 - laws_.tone)), k == 0 ? 4.0f : 2.0f);
		}
		if (kind_ == drum::Kind::Snare) {
			// THE SNARE'S THREE ENVELOPES AND ITS BELL: the wires' band moving with its sweep and cut at its gate, the click high
			// up at its own length, and the bell's glide drawn as a bright trace (dim while Bell is 0).
			const auto gated = [](double t, double gate) {
				return gate > 0.0 && t > gate ? std::pow(10.0, -3.0 * (t - gate) / 0.015) : 1.0;
			};
			for (int i = 0; i < kSteps; ++i) {
				const double t = secondsAt(plotLeft() + w * static_cast<float>(i) / kSteps);
				const double centre = drum::wireCentreHz(laws_.tone, laws_.hz)
				                      * std::exp2(laws_.wireSweepOctaves * std::exp(-t / std::max(1.0e-4, laws_.wireT60 / 6.0)));
				const double a = (0.3 + 0.7 * laws_.noise) * std::pow(10.0, -3.0 * t / laws_.wireT60) * gated(t, laws_.wireGate);
				canvas.setColor(alphaOf(kAccent, 0.55f * seen(a)));
				const float y0 = yOfHz(centre * 1.6);
				canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, y0, w / kSteps + 0.5f, yOfHz(centre / 1.6) - y0);
				const double c = laws_.snap * std::pow(10.0, -3.0 * t / laws_.clickT60);
				canvas.setColor(alphaOf(kInk, 0.5f * seen(c)));
				canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, yOfHz(12000.0), w / kSteps + 0.5f,
				            yOfHz(6500.0) - yOfHz(12000.0));
				const double bell = laws_.bellEndHz
				                    * std::pow(laws_.bellStartHz / laws_.bellEndHz, std::exp(-t / laws_.bellTau));
				canvas.setColor(alphaOf(kModulation, laws_.bellLevel > 0.0 ? 0.35f + 0.65f * static_cast<float>(laws_.bellLevel) : 0.18f));
				canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, yOfHz(bell) - 1.5f, w / kSteps + 0.5f, 3.0f);
				// THE SECOND BELL: riding the first's glide at Ratio (Linked) or still at its Hz (Free), fading with its ring.
				const double bell2 = laws_.bell2Linked ? bell * laws_.bell2Ratio : laws_.bell2Hz;
				const double ring = std::pow(10.0, -3.0 * t / std::max(1.0e-3, laws_.bell2T60));
				canvas.setColor(alphaOf(kModulation, laws_.bell2Level > 0.0
				                                         ? static_cast<float>(ring) * (0.35f + 0.65f * static_cast<float>(laws_.bell2Level))
				                                         : 0.12f));
				canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, yOfHz(bell2) - 1.0f, w / kSteps + 0.5f, 2.0f);
			}
		}
		else if (laws_.noise > 0.0f) {
			band(2.0 * laws_.hz, 0.3 * laws_.t60, laws_.noise);
		}
		break;
	}
	case drum::Kind::Metal:
		for (int i = 0; i < drum::kMetalOscillators; ++i) {
			line(laws_.hz * drum::metalRatio(i, laws_.metal), laws_.t60, 1.0 - laws_.noise, 2.0f);
		}
		band(drum::metalCentreHz(laws_.tone), laws_.t60, 1.0);
		break;
	case drum::Kind::Clap: band(drum::clapCentreHz(laws_.tone) * laws_.hz / drum::kindBaseHz(drum::Kind::Clap), laws_.t60, 1.0); break;
	case drum::Kind::String:
		for (int k = 1; k <= 12; ++k) {
			line(laws_.hz * k, laws_.t60 / std::pow(static_cast<double>(k), 0.5 + 0.5 * (1.0 - laws_.tone)), 1.0 / k,
			     k == 1 ? 4.0f : 2.0f);
		}
		break;
	default: break;
	}

	// THE AMPLITUDE ENVELOPE, filled, its peak at Level and its knee shaped by Drive through its curve (the engine's own transfer).
	const double peak = std::pow(10.0, std::clamp(levelDb_, -60.0, 6.0) / 20.0);
	const uint32_t fill = [] { // the envelope's fill: kValue at 35% over the plot's void
		uint32_t out = 0xff000000u;
		for (const int shift : { 0, 8, 16 }) {
			const float v = static_cast<float>((kVoid >> shift) & 0xffu);
			const float c = static_cast<float>((kValue >> shift) & 0xffu);
			out |= static_cast<uint32_t>(0.65f * v + 0.35f * c) << shift;
		}
		return out;
	}();

	for (int i = 0; i < kSteps; ++i) {
		const double t = secondsAt(plotLeft() + w * (static_cast<float>(i) + 0.5f) / kSteps);
		// A KICK HOLDS, THEN DECAYS, with its fishtail dipped in (the hit's own Hold and dipAt's curve).
		const bool kick = kind_ == drum::Kind::Kick;
		const double held = kick ? std::max(0.0, t - laws_.kickHold) : t;
		double a = std::pow(10.0, -3.0 * held / std::max(1.0e-3, laws_.t60)) * gatedAt(t, bodyGate);
		if (kick && laws_.kickDip > 0.0) {
			const double u = (t - 0.5 * laws_.kickDipTime) / laws_.kickDipTime;
			if (u > 0.0 && u < 1.0) {
				a *= 1.0 - laws_.kickDip * 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * u));
			}
		}
		if (laws_.drive > 0.0) {
			a = drum::driveTransfer(laws_.driveCurve, laws_.drive, a);
		}
		const double db = 20.0 * std::log10(std::max(1.0e-6, a * peak));
		const float top = yOfLevel(db);
		canvas.setColor(fill); // SOLID, not translucent: the steps overlap by half a pixel, and a translucent one shows the overlap as a comb
		canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, top, w / kSteps + 0.5f, envBottom() - top);
		canvas.setColor(kValue);
		canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, top - 1.0f, w / kSteps + 0.5f, 2.0f);
	}

	// THE TIME GRID: a faint line at each decade, over the traces and the envelope, its caption in the gap between them.
	for (const auto& [seconds, caption] : { std::pair { 0.001, "1ms" }, { 0.01, "10ms" }, { 0.1, "100ms" }, { 1.0, "1s" }, { 10.0, "10s" } }) {
		const float x = xOf(seconds);
		canvas.setColor(alphaOf(kLine, 0.3f));
		canvas.fill(x, specTop(), 1.0f, envBottom() - specTop());
		canvas.setColor(kMuted);
		canvas.text(caption, font(kKnobLabelSize), visage::Font::kLeft, x + 2.0f, specBottom() + 2.0f, 34.0f, 10.0f);
	}

	// THE HANDLES, each with its readout.
	const std::array<visage::Bounds, kHandleCount> labels = labelRects();
	const std::array<visage::Point, kHandleCount> marks = this->marks();
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (!handleShown(h)) {
			continue;
		}
		const visage::Point at = marks[i];
		if (modHandles_) {
			mods_[i].drawUnder(canvas, at.x, at.y, kHandleRadius); // the halo and the amount arc, under the dot
		}
		canvas.setColor(held_ == h ? kInk : kAccent);
		canvas.circle(at.x - kHandleRadius, at.y - kHandleRadius, 2.0f * kHandleRadius);
		if (modHandles_) {
			mods_[i].drawOver(canvas, at.x, at.y, kHandleRadius); // and the live dot over it
		}
		if (!labels_) {
			continue;
		}
		// ITS READOUT ON A BACKING, where labelRects put it: clear of every other label and handle, so no trace hides it.
		const visage::Bounds b = labels[i];
		canvas.setColor(alphaOf(kVoid, 0.8f));
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
		canvas.setColor(kInk);
		canvas.text(readoutOf(h), font(kKnobLabelSize), visage::Font::kLeft, b.x() + 3.0f, b.y(), b.width() - 3.0f, b.height());
	}
}

std::string HitPicture::readoutOf(Handle h) const {
		char text[48] = {};
		switch (h) {
		case Handle::Tune: std::snprintf(text, sizeof(text), "Tune %.0f Hz", laws_.hz); break;
		case Handle::SweepTime: std::snprintf(text, sizeof(text), "Sweep %.0f ms", 3000.0 * laws_.sweepTau); break;
		case Handle::Decay: std::snprintf(text, sizeof(text), "Decay %.0f ms", 1000.0 * laws_.t60); break;
		case Handle::Level: std::snprintf(text, sizeof(text), "Level %+.1f dB", levelDb_); break;
		case Handle::Drive:
			std::snprintf(text, sizeof(text), "Drive %.0f%%, tone %+.0f", 100.0 * laws_.drive, 100.0 * (2.0 * laws_.driveTone - 1.0));
			break;
		case Handle::WireDecay: std::snprintf(text, sizeof(text), "Wires %.0f ms", 1000.0 * laws_.wireT60); break;
		case Handle::ClickDecay: std::snprintf(text, sizeof(text), "Click %.1f ms", 1000.0 * laws_.clickT60); break;
		case Handle::BellStart: std::snprintf(text, sizeof(text), "Bell %.0f Hz", laws_.bellStartHz); break;
		case Handle::BellEnd: std::snprintf(text, sizeof(text), "to %.0f Hz, %.0f ms", laws_.bellEndHz, 3000.0 * laws_.bellTau); break;
		case Handle::Bell2:
			if (laws_.bell2Linked) {
				std::snprintf(text, sizeof(text), "Bell 2 x%.2f, %.0f ms", laws_.bell2Ratio, 1000.0 * laws_.bell2T60);
			}
			else {
				std::snprintf(text, sizeof(text), "Bell 2 %.0f Hz, %.0f ms", laws_.bell2Hz, 1000.0 * laws_.bell2T60);
			}
			break;
		case Handle::Hold: std::snprintf(text, sizeof(text), "Hold %.0f ms", 1000.0 * laws_.kickHold); break;
		case Handle::Dip: std::snprintf(text, sizeof(text), "Dip %.0f%% at %.0f ms", 100.0 * laws_.kickDip, 1000.0 * laws_.kickDipTime); break;
		case Handle::ClickSweep: std::snprintf(text, sizeof(text), "Click +%.1f oct, %.1f ms", laws_.clickSweepOctaves, 3000.0 * laws_.clickSweepTau); break;
		case Handle::StartPhase: std::snprintf(text, sizeof(text), "Phase %.0f deg", laws_.startPhase * 180.0 / std::numbers::pi); break;
		case Handle::Spread: std::snprintf(text, sizeof(text), "Spread %.0f ms", 1000.0 * laws_.spreadSeconds); break;
		case Handle::ClapBand: std::snprintf(text, sizeof(text), "HP %.0f Hz, Q %.1f", laws_.clapHighPassHz, laws_.clapQ); break;
		default: break;
		}
	return text;
}

std::array<visage::Bounds, HitPicture::kHandleCount> HitPicture::labelRects() const {
	const LabelKey key { width(), height(), dpiScale(), kind_, laws_, decaySeconds_, levelDb_, controls_, dragging_ };
	if (labelCached_ && key == labelKey_) {
		return labelCache_;
	}
	++labelBuilds_;
	labelCache_ = placeLabels();
	labelKey_ = key;
	labelCached_ = true;
	return labelCache_;
}

std::array<visage::Bounds, HitPicture::kHandleCount> HitPicture::placeLabels() const {
	// COLLISION-AVOIDING PLACEMENT: each shown handle's label takes the first of eight places around it that stays inside the
	// plot and clear of every label placed before it and of every handle; with none clear, the first that stays inside.
	std::array<visage::Bounds, kHandleCount> out {};
	std::vector<visage::Bounds> placed;
	std::vector<visage::Bounds> dots;
	const std::array<visage::Point, kHandleCount> marks = this->marks();
	for (size_t i = 0; i < kHandleCount; ++i) {
		if (handleShown(static_cast<Handle>(i))) {
			const visage::Point at = marks[i];
			dots.push_back({ at.x - kHandleRadius, at.y - kHandleRadius, 2.0f * kHandleRadius, 2.0f * kHandleRadius });
		}
	}
	const auto overlaps = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};
	const float h = 12.0f;
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto handle = static_cast<Handle>(i);
		if (!handleShown(handle)) {
			continue;
		}
		const visage::Point at = marks[i];
		const float w = textWidth(readoutOf(handle), font(kKnobLabelSize)) + 8.0f;
		const std::array<visage::Point, 8> offsets { { { 9.0f, -h - 4.0f }, { 9.0f, 4.0f }, { -w - 9.0f, -h - 4.0f },
			                                           { -w - 9.0f, 4.0f }, { 9.0f, -2.0f * h - 6.0f }, { 9.0f, h + 6.0f },
			                                           { -w * 0.5f, -h - 10.0f }, { -w * 0.5f, 10.0f } } };
		const auto inside = [this](const visage::Bounds& b) {
			return b.x() >= 0.0f && b.right() <= width() && b.y() >= 0.0f && b.bottom() <= height();
		};
		visage::Bounds chosen {};
		bool found = false;
		for (const visage::Point o : offsets) {
			const visage::Bounds b { at.x + o.x, at.y + o.y, w, h };
			if (!inside(b)) {
				continue;
			}
			if (!found) {
				chosen = b; // the fallback: the first that stays inside
				found = true;
			}
			const bool clear = std::none_of(placed.begin(), placed.end(), [&](const visage::Bounds& p) { return overlaps(p, b); })
			                   && std::none_of(dots.begin(), dots.end(), [&](const visage::Bounds& d) { return overlaps(d, b); });
			if (clear) {
				chosen = b;
				break;
			}
		}
		if (!found) {
			chosen = { std::clamp(at.x + 9.0f, 0.0f, std::max(0.0f, width() - w)), std::clamp(at.y - h - 4.0f, 0.0f, std::max(0.0f, height() - h)), w, h };
		}
		out[i] = chosen;
		placed.push_back(chosen);
	}
	return out;
}


} // namespace feathers::ui
