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

#include "plugin/ui/drum_panel.h"

#include "dsp/field_override.h"
#include "plugin/character_params.h"
#include "plugin/sound_travel.h"
#include "plugin/ui/theme.h"
#include "plugin/sound_hues.h"
#include "plugin/ui/celestial.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <utility>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kPad = 12.0f;
constexpr float kPictureHeight = 230.0f;
constexpr float kRowHeight = 30.0f;
constexpr float kPadCellWidth = 72.0f;
constexpr float kPadCellHeight = 48.0f;
constexpr float kPadGap = 6.0f;
/// A layer's terrain pad: square, at the body's left.
constexpr float kVectorPadSize = 148.0f;
/// The zone cells' face in the kit window's list rows.
constexpr float kZoneFace = 22.0f;
// THE PANEL IS EXACTLY ONE ROW OF EIGHT PADS WIDE, header's kMinimumWidth, and no wider (maximumSize).
static_assert(DrumPanel::kMinimumWidth == 2.0f * kPad + 8.0f * kPadCellWidth + 7.0f * kPadGap);

const ParamDesc& descOf(ParamId id) { return *findParam(static_cast<clap_id>(id)); }

/// THE PAD FX RAIL'S WORDS AND ITS CAST: the surface it follows IS the pad's chain (the parallel rack's `chainItself`).
constexpr const char* kPadLaneLabel = "PAD FX";
constexpr const char* kPadLaneLine = "after the pad, before the kit's mix";
constexpr const char* kPadLanePasses = "this pad's hits pass through untouched";
rack::InnerChain* padChainItself(void* surface) { return static_cast<rack::InnerChain*>(surface); }

uint32_t alphaOf(uint32_t colour, float alpha) {
	const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
	return (colour & 0x00ffffffu) | (a << 24);
}

} // namespace

//---------//
// DrumTab //
//---------//

DrumTab::DrumTab(std::function<void()> toggle) : toggle_(std::move(toggle)) {
	onMouseDown() = [this](const visage::MouseEvent&) {
		if (toggle_) {
			toggle_();
		}
	};
}

void DrumTab::draw(visage::Canvas& canvas) {
	canvas.setColor(open_ ? kAccent : kMuted);
	canvas.text("KIT", font(kControlSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	if (open_) {
		canvas.fill(width() * 0.5f - 8.0f, height() - 2.0f, 16.0f, 2.0f);
	}
}

void DrumTab::setOpen(bool open) {
	if (open != open_) {
		open_ = open;
		redraw();
	}
}

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

HitPicture::HitPicture() {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		fine_ = e.isShiftDown();
		press(e.position);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		fine_ = e.isShiftDown();
		drag(e.position);
	};
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
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

ParamControl* HitPicture::controlOf(Handle h) const {
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

ParamControl* HitPicture::secondOf(Handle h) const {
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
	ParamControl* c = controlOf(held_);
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
		groupHistory_ = c->recordsInto();
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
	ParamControl* c = heldControl_;
	ParamControl* second = heldSecond_;
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
	const auto reach = [&](const ParamControl* control, auto&& toValue) {
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
		canvas.setColor(held_ == h ? kInk : kAccent);
		canvas.circle(at.x - kHandleRadius, at.y - kHandleRadius, 2.0f * kHandleRadius);
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

//------------------//
// The settled pitch //
//------------------//

namespace drum_pitch {

namespace {
constexpr std::array<const char*, 12> kNoteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

NoteReading noteOf(double hz) {
	NoteReading out;
	if (!(hz > 0.0) || !std::isfinite(hz)) {
		return out;
	}
	const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
	const long n = std::lround(midi);
	out.cents = static_cast<int>(std::lround((midi - static_cast<double>(n)) * 100.0));
	const long within = ((n % 12) + 12) % 12;
	const long octave = (n - within) / 12 - 1;
	out.name = std::string(kNoteNames[static_cast<size_t>(within)]) + std::to_string(octave);
	return out;
}

std::string readoutText(const drum::SettledPitch& pitch) {
	if (!pitch.defined) {
		return {};
	}
	const NoteReading note = noteOf(pitch.hz);
	char text[64] = {};
	std::snprintf(text, sizeof(text), "%s%.1f Hz  %s %+dc", pitch.approximate ? "~" : "", pitch.hz, note.name.c_str(), note.cents);
	return text;
}

std::optional<double> parseHz(std::string_view text) {
	std::string t;
	for (const char c : text) {
		t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	const auto first = t.find_first_not_of(" \t");
	if (first == std::string::npos) {
		return std::nullopt;
	}
	t = t.substr(first);
	// A NOTE NAME: a letter A..G, an optional # or b, the octave, then optional cents ("+12", "-7c").
	if (t[0] >= 'a' && t[0] <= 'g') {
		static constexpr std::array<int, 7> kSemitone { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
		int semitone = kSemitone[static_cast<size_t>(t[0] - 'a')];
		size_t at = 1;
		if (at < t.size() && (t[at] == '#' || t[at] == 'b')) {
			semitone += t[at] == '#' ? 1 : -1;
			++at;
		}
		const char* begin = t.c_str() + at;
		char* end = nullptr;
		const long octave = std::strtol(begin, &end, 10);
		if (end == begin) {
			return std::nullopt; // no octave: not a note
		}
		double cents = 0.0;
		const char* rest = end;
		while (*rest == ' ') {
			++rest;
		}
		if (*rest == '+' || *rest == '-') {
			char* centsEnd = nullptr;
			cents = std::strtod(rest, &centsEnd);
			if (centsEnd == rest) {
				return std::nullopt;
			}
			rest = centsEnd;
			if (*rest == 'c') {
				++rest;
			}
		}
		while (*rest == ' ') {
			++rest;
		}
		if (*rest != '\0') {
			return std::nullopt;
		}
		const double midi = 12.0 * static_cast<double>(octave + 1) + static_cast<double>(semitone) + cents / 100.0;
		return 440.0 * std::exp2((midi - 69.0) / 12.0);
	}
	// A FREQUENCY: a number, then nothing or "hz" (and whatever follows it: the readout's own note text reads back).
	const char* begin = t.c_str();
	char* end = nullptr;
	const double hz = std::strtod(begin, &end);
	if (end == begin || !std::isfinite(hz) || hz <= 0.0) {
		return std::nullopt;
	}
	const char* rest = end;
	while (*rest == ' ') {
		++rest;
	}
	if (*rest == '\0' || (rest[0] == 'h' && rest[1] == 'z')) {
		return hz;
	}
	return std::nullopt;
}

} // namespace drum_pitch

std::optional<std::string> SettledPitchControl::editText() const {
	const drum::SettledPitch pitch = settled_();
	if (!pitch.defined || pitch.approximate) {
		return std::nullopt; // no one pitch to land: refuse rather than take a number for something else
	}
	return drum_pitch::readoutText(pitch);
}

std::optional<double> SettledPitchControl::parseText(std::string_view text, double shown) const {
	const drum::SettledPitch pitch = settled_();
	const std::optional<double> hz = drum_pitch::parseHz(text);
	if (!hz || !pitch.defined || pitch.approximate || !(pitch.hz > 0.0)) {
		return std::nullopt;
	}
	// TUNE IS A LOG OF THE BASE PITCH and every kind's settled pitch is proportional to the base, so the Tune that lands it on
	// `hz` is where Tune stands now plus the semitones between the two.
	const device::ParamDesc& d = tune_.desc();
	return std::clamp(shown + 12.0 * std::log2(*hz / pitch.hz), d.min, d.max);
}

const device::ParamDesc& PadSoloControl::desc() const {
	static const device::ParamDesc d { "solo", "Solo", 0.0, 1.0, 0.0, true, device::Unit::Number, {}, false, false,
		                               "Hear this pad alone: the preview the sound cards' S marks, so every other pad is silent while it is on. "
		                               "Not saved with the patch." };
	return d;
}

void PadSoloControl::setValue(double value) {
	if (value >= 0.5) {
		bridge_.setPreview(slot_);
	}
	else if (bridge_.preview().active() && bridge_.preview().slot == slot_) {
		bridge_.clearPreview();
	}
	if (changed_) {
		changed_();
	}
}

PitchReadout::PitchReadout(SettledPitchControl& control) : control_(control) {
	onMouseDown() = [this](const visage::MouseEvent& e) { beginHand(e.position.y, e.isShiftDown()); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { moveHand(e.position.y, e.isShiftDown()); };
	onMouseUp() = [this](const visage::MouseEvent&) { endHand(); };
	installTooltipHover(*this, *this);
}

void PitchReadout::resized() { typeIn_.place(typeInBounds()); }

const char* PitchReadout::tooltipAt(visage::Point) const {
	return "The pitch this hit settles on, at the pad's resting pitch, its Tune and the terrain's. Click to type a note (A1, C#4) "
	       "or a frequency; drag to move Tune a semitone at a time, Shift for fine.";
}

void PitchReadout::refresh(const std::string& text) {
	if (!isVisible()) {
		drawn_ = text; // nothing is drawn while it is hidden, so nothing is owed a repaint
		return;
	}
	if (text != drawn_) {
		redraw();
	}
}

void PitchReadout::draw(visage::Canvas& canvas) {
	drawn_ = control_.text();
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 4.0f);
	canvas.setColor(alphaOf(kLine, 0.8f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), 4.0f, 1.0f);
	canvas.setColor(kInk);
	canvas.text(drawn_, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
}

void PitchReadout::beginHand(float y, bool fine) {
	const bool closedField = typeIn_.closeIfOpen();
	pressed_ = !closedField;
	dragged_ = false;
	fine_ = fine;
	anchorY_ = y;
	anchorValue_ = control_.value();
}

void PitchReadout::moveHand(float y, bool fine) {
	if (!pressed_) {
		return;
	}
	if (!dragged_) {
		if (std::fabs(anchorY_ - y) < 3.0f) {
			return; // a click that wandered a pixel is still a click
		}
		dragged_ = true;
		control_.beginGesture();
	}
	if (fine != fine_) {
		// SHIFT CHANGING MID-DRAG RE-ANCHORS (the knob's rule), so nothing jumps.
		fine_ = fine;
		anchorY_ = y;
		anchorValue_ = control_.value();
	}
	const double semitones = static_cast<double>(anchorY_ - y) / static_cast<double>(kPixelsPerSemitone);
	control_.setValue(fine_ ? anchorValue_ + semitones * static_cast<double>(kFineAdjust) : std::round(anchorValue_ + semitones));
	redraw();
}

void PitchReadout::endHand() {
	if (!pressed_) {
		return;
	}
	pressed_ = false;
	if (dragged_) {
		control_.endGesture();
		dragged_ = false;
		return;
	}
	// A CLICK TYPES: the field opens on the readout's own text, a note or a frequency lands the settled pitch there.
	typeIn_.beginReset(control_, false, true);
	(void)typeIn_.open(typeInBounds());
}

void PitchReadout::clickForTest() {
	beginHand(10.0f, false);
	endHand();
}

void PitchReadout::dragForTest(float dy, bool fine) {
	beginHand(100.0f, fine);
	moveHand(100.0f - dy, fine);
	endHand();
}

//------------//
// LayerTabs  //
//------------//

LayerTabs::LayerTabs(std::function<void(size_t)> choose) : choose_(std::move(choose)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const int tab = tabAt(e.position);
		if (tab >= 0 && choose_) {
			choose_(static_cast<size_t>(tab));
		}
	};
}

int LayerTabs::tabAt(visage::Point p) const {
	if (p.y < 0.0f || p.y > height() || p.x < 0.0f || p.x >= 2.0f * kTabWidth) {
		return -1;
	}
	return p.x < kTabWidth ? 0 : 1;
}

void LayerTabs::setShown(size_t layer) {
	if (layer != shown_) {
		shown_ = layer;
		redraw();
	}
}

void LayerTabs::draw(visage::Canvas& canvas) {
	for (size_t i = 0; i < 2; ++i) {
		const float x = static_cast<float>(i) * kTabWidth;
		const bool lit = i == shown_;
		canvas.setColor(lit ? kSurfaceRaised : kSurface);
		canvas.roundedRectangle(x + 1.0f, 0.0f, kTabWidth - 2.0f, height(), 4.0f);
		canvas.setColor(lit ? kAccent : kMuted);
		canvas.text(i == 0 ? "Layer 1" : "Layer 2", font(kControlSize), visage::Font::kCenter, x, 0.0f, kTabWidth, height());
		if (lit) {
			canvas.fill(x + 10.0f, height() - 2.0f, kTabWidth - 20.0f, 2.0f);
		}
	}
}

//-----------//
// DrumPanel //
//-----------//

DrumPanel::DrumPanel(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection, Notifier* notifier,
                     FrameWatchers* previewWatchers)
    : bridge_(bridge), previewWatchers_(previewWatchers), selection_(selection) {
	mode_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::DrumMode), kDrumModeNames, notifier);
	pitchedKindControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumPitchedKind));
	pitchedKind_ = std::make_unique<ValueBox>(*pitchedKindControl_, "Kind");
	seedControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumSeed));
	seed_ = std::make_unique<ValueBox>(*seedControl_, "Seed");
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumPitchedKind), pitchedKind_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumSeed), seed_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this));
	if (previewWatchers_ != nullptr) {
		watches_.push_back(previewWatchers_->watch(this));
	}
	noRotate_ = std::make_unique<ParamControl>(bridge, nullptr);
	tabs_ = std::make_unique<LayerTabs>([this](size_t layer) { chooseLayer(layer); });
	addChild(&picture_);
	addChild(tabs_.get());
	addChild(mode_.get());
	addChild(pitchedKind_.get());
	addChild(seed_.get());

	// ONE ParamControl PER WIDGET (the spectrum panel's pattern), every one handed the modulation state when the panel is bound.
	const auto control = [&](std::unique_ptr<ParamControl>& out, ParamId id, visage::Frame* watcher) {
		const ParamDesc& d = descOf(id);
		out = std::make_unique<ParamControl>(bridge, &d);
		allControls_.push_back(out.get());
		if (watcher != nullptr) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), watcher));
		}
	};
	const auto box = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<ValueBox>& widget, ParamId id, const char* label) {
		control(out, id, nullptr);
		widget = std::make_unique<ValueBox>(*out, label);
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), widget.get()));
		addChild(widget.get());
	};
	const auto pill = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<FaderPill>& widget, ParamId id, const char* caption) {
		control(out, id, nullptr);
		widget = std::make_unique<FaderPill>(*out, caption);
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), widget.get()));
		addChild(widget.get());
	};
	const auto segmented = [&](std::unique_ptr<SegmentedControl>& out, ParamId id, const auto& names) { // (a span keeps a reference to them)
		out = std::make_unique<SegmentedControl>(bridge, registry, descOf(id), names, notifier);
		addChild(out.get());
	};

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		// THE PAD-LEVEL CONTROLS: Note, Choke, Voices, Trigger, Pan, Bend, Release (Level is the sound card's, and layer one's Level handle).
		box(set.noteControl, set.note, drumParam(slot, DrumControl::Note), "Note");
		box(set.chokeControl, set.choke, drumParam(slot, DrumControl::Choke), "Choke");
		segmented(set.voices, drumParam(slot, DrumControl::Voices), kDrumVoicesNames);
		segmented(set.trigger, drumParam(slot, DrumControl::Trigger), kDrumTriggerNames);
		pill(set.panControl, set.pan, drumParam(slot, DrumControl::Pan), "Pan");
		pill(set.releaseControl, set.release, drumParam(slot, DrumControl::Release), "Release");
		pill(set.bendControl, set.bend, drumParam(slot, DrumControl::Bend), "Bend");
		// MUTE is the pad's own Mute; SOLO the preview of this pad alone (the cards' S), which the synth already honours.
		control(set.muteControl, drumParam(slot, DrumControl::Mute), nullptr);
		set.mute = std::make_unique<SwitchChip>(*set.muteControl, "Mute");
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Mute)), set.mute.get()));
		addChild(set.mute.get());
		set.soloControl = std::make_unique<PadSoloControl>(bridge, slot, [this] {
			if (previewWatchers_ != nullptr) {
				previewWatchers_->notify();
			}
		});
		set.solo = std::make_unique<SwitchChip>(*set.soloControl, "Solo");
		addChild(set.solo.get());
		// The pad's kind and Trigger decide what is drawn, so a move of either re-lays the panel.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Kind)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Trigger)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::LayerKind)), this));

		for (size_t layer = 0; layer < 2; ++layer) {
			LayerSet& L = set.layer[layer];
			const auto id = [&](DrumControl c) { return drumLayerParam(slot, layer, c); };
			// THE HANDLES' CONTROLS: the picture writes them for the shown layer, and watches every layer's, so a host move
			// redraws it and each of them is reached by a hand (the reachability guard asks the registry).
			const auto handle = [&](std::unique_ptr<ParamControl>& out, DrumControl c) { control(out, id(c), &picture_); };
			handle(L.tune, DrumControl::Tune);
			handle(L.sweepTime, DrumControl::SweepTime);
			handle(L.decay, DrumControl::Decay);
			handle(L.drive, DrumControl::Drive);
			handle(L.driveTone, DrumControl::DriveTone);
			handle(L.wireDecay, DrumControl::WireDecay);
			handle(L.clickDecay, DrumControl::ClickDecay);
			handle(L.bellStart, DrumControl::BellStart);
			handle(L.bellEnd, DrumControl::BellEnd);
			handle(L.bellTime, DrumControl::BellTime);
			handle(L.bell2Hz, DrumControl::Bell2Hz);
			handle(L.bell2Time, DrumControl::Bell2Time);
			handle(L.hold, DrumControl::KickHold);
			handle(L.dip, DrumControl::KickDip);
			handle(L.dipTime, DrumControl::KickDipTime);
			handle(L.clickSweep, DrumControl::ClickSweep);
			handle(L.clickSweepTime, DrumControl::ClickSweepTime);
			handle(L.startPhase, DrumControl::StartPhase);
			handle(L.spread, DrumControl::Spread);
			handle(L.clapQ, DrumControl::ClapQ);
			handle(L.clapHighPass, DrumControl::ClapHighPass);
			// THE LEVEL HANDLE: the pad's Level on layer one, the layer's own Level (on top of the pad's) on layer two.
			control(L.level, layer == 0 ? drumParam(slot, DrumControl::Level) : drumParam(slot, DrumControl::LayerLevel), &picture_);
			// THE KIND: the pad's own (a kind of the six), or the layer's, whose first choice is Off.
			box(L.kindControl, L.kind, drumLayerKindParam(slot, layer), "Kind");
			box(L.countControl, L.count, id(DrumControl::Count), "Claps");
			segmented(L.driveCurve, id(DrumControl::DriveCurve), kDrumDriveCurveNames);
			segmented(L.pitchMode, id(DrumControl::PitchMode), kDrumPitchModeNames);
			pill(L.tuneControl, L.tunePill, id(DrumControl::Tune), "Tune");
			L.tunePill->setWholeSteps(true); // semitones, as the readout and the picture's handle
			pill(L.velocityNoiseControl, L.velocityNoise, id(DrumControl::VelocityNoise), "Vel Noise");
			pill(L.widthControl, L.width, id(DrumControl::Width), "Width");
			if (layer == 1) {
				pill(L.levelPillControl, L.levelPill, drumParam(slot, DrumControl::LayerLevel), "Level");
				pill(L.delayControl, L.delay, drumParam(slot, DrumControl::LayerDelay), "Delay");
				pill(L.highPassControl, L.highPass, drumParam(slot, DrumControl::LayerHighPass), "HP");
			}
			// THE SEVEN FIELD SCALERS of this layer (Pitch .. Metal), each drawn only where its kind reads it.
			static constexpr std::array<DrumControl, 7> kFields { DrumControl::FieldPitch, DrumControl::FieldLength, DrumControl::FieldSweep,
				                                                  DrumControl::FieldTone,  DrumControl::FieldNoise,  DrumControl::FieldSnap,
				                                                  DrumControl::FieldMetal };
			static constexpr std::array<const char*, 7> kFieldCaptions { "Pitch", "Length", "Sweep", "Tone", "Noise", "Snap", "Metal" };
			for (size_t f = 0; f < kFields.size(); ++f) {
				pill(L.fieldControl[f], L.field[f], id(kFields[f]), kFieldCaptions[f]);
			}
			// THE ZONE CONTROLS (drum proposal §14): one coordinate each, its zone named in a dropdown, in the list row's layout.
			const auto zone = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<ZoneCoordCell>& cell, DrumControl c,
			                      const char* caption, auto names) {
				control(out, id(c), nullptr);
				std::vector<ZoneSelect::ZoneName> zoneNames;
				for (const char* n : names) {
					zoneNames.push_back({ n, "" });
				}
				cell = std::make_unique<ZoneCoordCell>(*out, caption, std::move(zoneNames), ZoneSelect::kCoordinatePerLap);
				cell->setRowLayout(true);
				watches_.push_back(registry.watchToken(static_cast<clap_id>(id(c)), cell.get()));
				addChild(cell.get());
			};
			zone(L.velocityZoneControl, L.velocityZone, DrumControl::VelocityZone, "Velocity", drum::kVelocityZoneNames);
			zone(L.wiresControl, L.wires, DrumControl::Wires, "Wires", drum::kWiresZoneNames);
			zone(L.bellsControl, L.bells, DrumControl::Bells, "Bells", drum::kBellsZoneNames);
			// The Bells zone decides whether the second bell is linked, which is what its handle writes, so a move re-lays.
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::Bells)), this));
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::PitchMode)), this));
			// THE LAYER'S VECTOR: its terrain pad, and Lock beside it.
			control(L.lockControl, id(DrumControl::Lock), nullptr);
			L.lock = std::make_unique<SwitchChip>(*L.lockControl, "Lock");
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::Lock)), L.lock.get()));
			addChild(L.lock.get());
			// THE VECTOR'S CONTROLS FIRST: the pad reads its row as it is built.
			control(L.offsetXControl, id(DrumControl::OffsetX), nullptr);
			control(L.offsetYControl, id(DrumControl::OffsetY), nullptr);
			L.vectorContext.slot = static_cast<uint8_t>(slot);
			L.vectorContext.layer = static_cast<uint8_t>(layer);
			L.vectorPad = std::make_unique<DrumVectorPad>(
			    [this, slot, layer](size_t index, float value) {
				    LayerSet& l = pads_[slot].layer[layer];
				    (index == 0 ? l.offsetXControl : l.offsetYControl)->setValue(static_cast<double>(value));
			    },
			    [this, slot, layer](size_t field) {
				    const LayerSet& l = pads_[slot].layer[layer];
				    return static_cast<float>((field == 0 ? l.offsetXControl : l.offsetYControl)->value());
			    },
			    *noRotate_);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::OffsetX)), L.vectorPad.get()));
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::OffsetY)), L.vectorPad.get()));
			L.vectorPad->context = [this, slot, layer]() -> const DrumVectorContext& { return pads_[slot].layer[layer].vectorContext; };
			// ONE DRAG OF THE PAD IS ONE UNDO STEP, as the picture's two-way handles are: both vector parameters in one group.
			L.vectorPad->onHoldBegan = [this, slot, layer] {
				LayerSet& l = pads_[slot].layer[layer];
				l.vectorGroup = l.offsetXControl->recordsInto();
				if (l.vectorGroup != nullptr) {
					l.vectorGroup->beginGroup("Move layer");
				}
				l.offsetXControl->beginGesture();
				l.offsetYControl->beginGesture();
			};
			L.vectorPad->onHoldEnded = [this, slot, layer] {
				LayerSet& l = pads_[slot].layer[layer];
				l.offsetXControl->endGesture();
				l.offsetYControl->endGesture();
				if (l.vectorGroup != nullptr) {
					l.vectorGroup->endGroup();
					l.vectorGroup = nullptr;
				}
			};
			addChild(L.vectorPad.get());
			// THE SETTLED PITCH, over this layer's Tune: its own text, a hand on it.
			L.settledControl = std::make_unique<SettledPitchControl>(*L.tuneControl, [this, slot, layer] { return settledOf(slot, layer); });
			L.readout = std::make_unique<PitchReadout>(*L.settledControl);
			addChild(L.readout.get());
		}
	}
	// THE PAD FX RAIL (drum proposal §17 P3): the focused pad's lane, hanging off the right side as SKLITTER's inner chain
	// does. Built with the panel, CLOSED (SKLITTER's rule: a chain is not the instrument, and a pad's tile wears a dot while
	// its lane holds anything); one click opens it.
	lane_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kPadLaneLabel, kPadLaneLine, &padChainItself }, registry, notifier);
	addChild(lane_.get());
	lane_->onRailWidthChanged([this]() {
		resized();
		redraw();
		minimumSizeChanged();
	});
	(void)followLane();
	selectionWatch_ = selection_.watch(this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!kitMode()) {
			return;
		}
		for (size_t pad = 0; pad < kMaxSounds; ++pad) {
			if (padBounds(pad).contains(e.position)) {
				selection_.select(pad);
				sync();
				redraw();
				return;
			}
		}
	};
	sync();
}

DrumPanel::~DrumPanel() = default;

std::string DrumPanel::headerTitle() const {
	if (!kitMode()) {
		return "Drum · Pitched";
	}
	const size_t pad = std::min(selection_.slot(), kMaxSounds - 1);
	// THE PAD AND ITS KIND NOW (not the lane it started as): a pad switched to a Snare says Snare.
	const auto kind = static_cast<size_t>(kindOf(pad));
	return std::string("Drum Kit · ") + static_cast<char>('A' + pad) + " · " + kDrumKindNames[std::min(kind, kDrumKindNames.size() - 1)];
}

void DrumPanel::modulationBound(ModState* mod, EditHistory* history) {
	for (ParamControl* c : allControls_) {
		c->setModulation(mod, history);
	}
}

bool DrumPanel::kitMode() const {
	return steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	       == static_cast<size_t>(DrumMode::Kit);
}

size_t DrumPanel::shownPad() const { return kitMode() ? std::min(selection_.slot(), kMaxSounds - 1) : 0; }

drum::Kind DrumPanel::kindOf(size_t pad) const {
	const clap_id id = kitMode() ? static_cast<clap_id>(drumParam(pad, DrumControl::Kind))
	                             : static_cast<clap_id>(ParamId::DrumPitchedKind);
	return static_cast<drum::Kind>(steppedIndex(bridge_.value(id), kDrumKindNames.size()));
}

std::optional<drum::Kind> DrumPanel::layerKindOf(size_t pad, size_t layer) const {
	if (layer == 0) {
		return kindOf(pad);
	}
	// LAYER TWO'S KINDS ARE Off, THEN Kick..Clap in Kind's own order, so index - 1 is the Kind.
	const size_t index = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::LayerKind))), kDrumLayerKindNames.size());
	if (!kitMode() || index == 0) {
		return std::nullopt;
	}
	return static_cast<drum::Kind>(index - 1);
}

Character DrumPanel::characterNow() const {
	// THE PAD WHERE IT REALLY STANDS (the honesty rule: what you see is what the pad plays): the character the synth builds, by
	// WaveDisplay's road — modulated values, the played travel (drift and pulls), the planes, residue, gamma and the likes'
	// warp. Measured cost: seven triangle reads a sound.
	const auto read = [this](ParamId id) { return bridge_.modulated(static_cast<clap_id>(id)); };
	Character c = characterFromParams(read, OscType::Drum, bridge_.tuning(), bridge_.puckVelocity(),
	                                  playedTravel(bridge_.soundTravel(), bridge_.travelPulls()), bridge_.soundOrientations(),
	                                  bridge_.soundResidue(), bridge_.soundGamma(), bridge_.preview(), EarSide::Head);
	bool any = false;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		likeTable_[slot] = bridge_.likeEngineOf(slot) == static_cast<uint8_t>(OscType::Drum) ? bridge_.likeWarpOf(slot) : LikeWarp {};
		any = any || !likeTable_[slot].identity();
	}
	if (any) {
		c.likes = { &likeTable_, bridge_.likeSerial() + 1 };
	}
	return c;
}

drum::Fields DrumPanel::fieldsFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	if (layer == 0) {
		return built.pads[built.kit ? pad : 0];
	}
	return built.kit ? built.layers[pad] : drum::Fields {};
}

drum::Controls DrumPanel::controlsOf(size_t pad, size_t layer) const {
	// THE VOICE'S MAPPING (Synth::fillDrumKit's `fillControls`): the layer's own controls over its real fields.
	const auto v = [&](DrumControl c) { return bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, c))); };
	const bool kit = kitMode();
	drum::Controls c;
	c.decaySeconds = std::exp2(v(DrumControl::Decay));
	c.sweepSeconds = std::exp2(v(DrumControl::SweepTime));
	c.drive = v(DrumControl::Drive);
	c.count = static_cast<int>(std::lround(v(DrumControl::Count)));
	c.spreadSeconds = std::exp2(v(DrumControl::Spread));
	c.pitchRangeSemitones = kit ? 3.0 : 0.5;
	c.velocityZone = v(DrumControl::VelocityZone);
	c.wires = v(DrumControl::Wires);
	c.bells = v(DrumControl::Bells);
	c.wireDecaySeconds = std::exp2(v(DrumControl::WireDecay));
	c.clickDecaySeconds = std::exp2(v(DrumControl::ClickDecay));
	c.bellStartHz = std::exp2(v(DrumControl::BellStart));
	c.bellEndHz = std::exp2(v(DrumControl::BellEnd));
	c.bellSeconds = std::exp2(v(DrumControl::BellTime));
	c.kickHoldSeconds = v(DrumControl::KickHold) / 1000.0;
	c.kickDip = v(DrumControl::KickDip);
	c.kickDipSeconds = v(DrumControl::KickDipTime) / 1000.0;
	c.clickSweepOctaves = v(DrumControl::ClickSweep);
	c.clickSweepSeconds = std::exp2(v(DrumControl::ClickSweepTime));
	c.startPhaseDegrees = v(DrumControl::StartPhase);
	c.driveCurve = static_cast<int>(steppedIndex(v(DrumControl::DriveCurve), kDrumDriveCurveNames.size()));
	c.driveTone = v(DrumControl::DriveTone);
	c.clapQ = v(DrumControl::ClapQ);
	c.clapHighPassHz = std::exp2(v(DrumControl::ClapHighPass));
	c.bell2Hz = std::exp2(v(DrumControl::Bell2Hz));
	c.bell2Seconds = std::exp2(v(DrumControl::Bell2Time));
	c.velocityNoise = v(DrumControl::VelocityNoise);
	c.width = v(DrumControl::Width);
	// THE TERRAIN'S PITCH MODE: Kit mode alone (Pitched mode keeps the +-0.5 semitone fine pitch as it always was).
	c.pitchMode = kit ? static_cast<int>(steppedIndex(v(DrumControl::PitchMode), kDrumPitchModeNames.size())) : 0;
	return c;
}

namespace {
/// THE KIT-WIDE TRANSPOSE in semitones, as the synth sums it (Synth::fillDrumKit): the Engine rail's Oct, St, Coarse and Fine.
double kitTransposeOf(const ParamBridge& bridge) {
	const auto v = [&](ParamId id) { return bridge.modulated(static_cast<clap_id>(id)); };
	return 12.0 * v(ParamId::TransposeOct) + v(ParamId::TransposeSemi) + v(ParamId::Coarse) + v(ParamId::FineTune) / 100.0;
}
} // namespace

drum::Laws DrumPanel::lawsFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	const drum::Kind k = layerKindOf(pad, layer).value_or(kindOf(pad));
	const double tune = bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Tune)));
	// THE RESTING PITCH: a kit's kind's own (the kit's transpose in), or a C3 in Pitched mode; then Tune.
	const double base = kitMode() ? drum::restingBaseHz(k, tune, kitTransposeOf(bridge_)) : 130.8127826502993 * std::exp2(tune / 12.0);
	return drum::lawsFor(k, fieldsFrom(pad, layer, built), controlsOf(pad, layer), base, 0.8);
}

drum::SettledPitch DrumPanel::settledFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	const std::optional<drum::Kind> k = layerKindOf(pad, layer);
	if (!k || !kitMode()) {
		return {};
	}
	const double tune = bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Tune)));
	return drum::settledPitch(*k, fieldsFrom(pad, layer, built), controlsOf(pad, layer), drum::restingBaseHz(*k, tune, kitTransposeOf(bridge_)));
}

drum::SettledPitch DrumPanel::settledOf(size_t pad, size_t layer) const {
	return settledFrom(pad, layer, buildDrumFor(characterNow()));
}

void DrumPanel::refreshVectorContext(size_t pad, size_t layer, const Character& character) {
	DrumVectorContext& ctx = pads_[pad].layer[layer].vectorContext;
	ctx.character = character;
	ctx.slot = static_cast<uint8_t>(pad);
	ctx.layer = static_cast<uint8_t>(layer);
	ctx.kind = layerKindOf(pad, layer).value_or(kindOf(pad));
}

visage::Bounds DrumPanel::padBounds(size_t pad) const {
	// ONE ROW OF EIGHT, A to H left to right (the kit window's layout, by design): the panel is exactly as wide as
	// the row, and the pad's controls sit under it.
	const size_t col = pad % kMaxSounds;
	const float top = kPad + kPictureHeight + kPad + kRowHeight + kPad;
	return { kPad + static_cast<float>(col) * (kPadCellWidth + kPadGap), top,
		     kPadCellWidth, kPadCellHeight };
}

void DrumPanel::chooseLayer(size_t layer) {
	layer = std::min<size_t>(layer, 1);
	if (layer == layerShown_) {
		return;
	}
	layerShown_ = layer;
	tabs_->setShown(layer);
	shownPad_ = kMaxSounds; // lay out again for the other tab
	sync();
	redraw();
}

bool DrumPanel::sync() {
	const bool kit = kitMode();
	const size_t pad = shownPad();
	const size_t layer = kit ? layerShown_ : 0; // Pitched mode has layer one alone
	const std::optional<drum::Kind> shownKindOpt = layerKindOf(pad, layer);
	const bool layerOn = shownKindOpt.has_value();
	const drum::Kind kind = shownKindOpt.value_or(kindOf(pad));
	const drum::ControlUse uses = drum::kindControls(kind);
	const drum::FieldUse reads = drum::kindReads(kind);
	const bool gate = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Trigger))),
	                               kDrumTriggerNames.size())
	                  == 1;
	PadSet& mine = pads_[pad];
	LayerSet& L = mine.layer[layer];
	const Character character = layerOn ? characterNow() : Character {};
	const DrumCharacter built = layerOn ? buildDrumFor(character) : DrumCharacter {};
	const drum::SettledPitch settled = layerOn ? settledFrom(pad, layer, built) : drum::SettledPitch {};
	HitPicture::Controls handles;
	if (layerOn) {
		handles.tune = L.tune.get();
		handles.decay = L.decay.get();
		handles.level = L.level.get();
		handles.sweepTime = uses.sweepTime ? L.sweepTime.get() : nullptr;
		handles.drive = uses.drive ? L.drive.get() : nullptr;
		handles.driveTone = uses.drive ? L.driveTone.get() : nullptr;
		if (uses.snareEnvelopes) {
			handles.wireDecay = L.wireDecay.get();
			handles.clickDecay = L.clickDecay.get();
		}
		if (uses.bell) {
			handles.bellStart = L.bellStart.get();
			handles.bellEnd = L.bellEnd.get();
			handles.bellTime = L.bellTime.get();
			handles.bell2 = L.bell2Hz.get();
			handles.bell2Time = L.bell2Time.get();
		}
		if (uses.kickShape) {
			handles.hold = L.hold.get();
			handles.dip = L.dip.get();
			handles.dipTime = L.dipTime.get();
			handles.clickSweep = L.clickSweep.get();
			handles.clickSweepTime = L.clickSweepTime.get();
			handles.startPhase = L.startPhase.get();
		}
		if (uses.spread) {
			handles.spread = L.spread.get();
		}
		if (uses.clapFilter) {
			handles.clapHighPass = L.clapHighPass.get();
			handles.clapQ = L.clapQ.get();
		}
		const double levelDb = bridge_.modulated(static_cast<clap_id>(layer == 0 ? drumParam(pad, DrumControl::Level)
		                                                                          : drumParam(pad, DrumControl::LayerLevel)));
		picture_.show(kind, lawsFrom(pad, layer, built),
		              std::exp2(bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Decay)))), levelDb,
		              handles);
		refreshVectorContext(pad, layer, character);
		L.readout->refresh(drum_pitch::readoutText(settled));
	}
	else {
		picture_.show(kind, drum::Laws {}, 0.5, 0.0, handles);
	}
	if (pad == shownPad_ && layer == shownLayer_ && kit == shownKit_ && kind == shownKind_ && layerOn == shownLayerOn_
	    && gate == shownGate_ && settled.defined == shownSettled_) {
		return false;
	}
	shownPad_ = pad;
	shownLayer_ = layer;
	shownKit_ = kit;
	shownKind_ = kind;
	shownLayerOn_ = layerOn;
	shownGate_ = gate;
	shownSettled_ = settled.defined;
	padsShown_ = kit;
	const auto show = [](auto* frame, bool shown) {
		if (frame->isVisible() != shown) {
			frame->setVisible(shown);
		}
		// A HIDDEN CONTROL IS A DEAD ONE TOO, not merely undrawn (the Explore surround's rule), where the widget can say so.
		if constexpr (requires { frame->setEnabled(shown); }) {
			frame->setEnabled(shown);
		}
	};
	show(pitchedKind_.get(), !kit);
	show(tabs_.get(), kit);
	tabs_->setShown(layerShown_);
	show(&picture_, layerOn);
	// THE LAYOUT: the picture with its handles across the top; under it the engine row (Mode, the Pitched Kind, Seed); then
	// the pads, the pad-level controls, the layer tabs, and the shown tab's body.
	// THE CONTENT IS THE PAD ROW'S WIDTH; the pad fx rail hangs to its right (Kit mode alone, drum proposal §17 P3).
	const float rail = railWidthNow();
	show(lane_.get(), kit);
	lane_->setBounds(std::max(0.0f, width() - rail), 0.0f, rail, height());
	const float w = width() > 0.0f ? std::max(0.0f, width() - rail) : kMinimumWidth;
	picture_.setBounds(kPad, kPad, w - 2.0f * kPad, kPictureHeight);
	const float rowY = kPad + kPictureHeight + kPad;
	mode_->setBounds(kPad, rowY, 150.0f, 24.0f);
	pitchedKind_->setBounds(kPad + 160.0f, rowY - 4.0f, 90.0f, ValueBox::kHeight);
	seed_->setBounds(w - kPad - 80.0f, rowY - 4.0f, 80.0f, ValueBox::kHeight);

	// A ROW-WRAPPING FLOW: items in a line, each `rowH` tall and centred in it, wrapping when the next would pass the right edge.
	struct Flow {
		float x0, xMax, x, y, rowH, gap;
		void place(visage::Frame* f, float width, float height) {
			if (x > x0 && x + width > xMax) {
				x = x0;
				y += rowH + 2.0f;
			}
			f->setBounds(x, y + (rowH - height) * 0.5f, width, height);
			x += width + gap;
		}
		[[nodiscard]] float bottom() const { return y + rowH; }
	};
	const float padLevelY = kit ? padBounds(0).bottom() + kPad : rowY + kRowHeight + kPad;
	float bodyBottom = padLevelY;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		const bool own = slot == pad;
		// EVERY CONTROL OF A PAD THAT IS NOT THE SHOWN ONE IS HIDDEN (and dead), both layers'.
		for (size_t l = 0; l < 2; ++l) {
			LayerSet& s = set.layer[l];
			if (own && l == layer) {
				continue;
			}
			for (visage::Frame* f : std::initializer_list<visage::Frame*> {
			         s.kind.get(), s.count.get(), s.driveCurve.get(), s.pitchMode.get(), s.velocityZone.get(), s.wires.get(),
			         s.bells.get(), s.tunePill.get(), s.velocityNoise.get(), s.width.get(), s.levelPill.get(), s.delay.get(),
			         s.highPass.get(), s.lock.get(), s.vectorPad.get(), s.readout.get() }) {
				if (f != nullptr) {
					show(f, false);
				}
			}
			for (auto& f : s.field) {
				show(f.get(), false);
			}
		}
		if (!own) {
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { set.note.get(), set.choke.get(), set.voices.get(),
			                                                                set.trigger.get(), set.pan.get(), set.release.get(),
			                                                                set.bend.get(), set.mute.get(), set.solo.get() }) {
				show(f, false);
			}
			continue;
		}
		// THE PAD-LEVEL CONTROLS, one flowing line (two when a Gate pad adds its Release): Note, Choke, Voices (Kit mode), Trigger,
		// Pan, Bend (Kit), Release (Gate).
		Flow flow { kPad, w - kPad, kPad, padLevelY, ValueBox::kHeight, 6.0f };
		const auto seat = [&](visage::Frame* f, bool shown, float width, float height) {
			show(f, shown);
			if (shown) {
				flow.place(f, width, height);
			}
		};
		seat(set.note.get(), kit, 52.0f, ValueBox::kHeight);
		seat(set.choke.get(), kit, 52.0f, ValueBox::kHeight);
		seat(set.voices.get(), kit, 96.0f, 22.0f);
		seat(set.trigger.get(), true, 128.0f, 22.0f);
		seat(set.pan.get(), true, 92.0f, 22.0f);
		seat(set.bend.get(), kit, 92.0f, 22.0f); // the pitch bend's share (decision 8): a kit pad's alone; Pitched mode follows the voice
		seat(set.release.get(), gate, 92.0f, 22.0f);
		seat(set.mute.get(), kit, 54.0f, SwitchChip::kHeight); // the pad's own Mute and the preview of it alone (kit pads only)
		seat(set.solo.get(), kit, 54.0f, SwitchChip::kHeight);
		float y = flow.bottom() + 10.0f;
		LayerSet& s = set.layer[layer];
		// THE LAYER TABS, and beside them the shown layer's Kind (layer two's with Off) and, on layer two, Level, Delay and High Pass.
		if (kit) {
			tabs_->setBounds(kPad, y, 2.0f * LayerTabs::kTabWidth, LayerTabs::kHeight);
			Flow top { 2.0f * LayerTabs::kTabWidth + kPad + 12.0f, w - kPad, 0.0f, y - 2.0f, ValueBox::kHeight, 6.0f };
			top.x = top.x0;
			show(s.kind.get(), true);
			top.place(s.kind.get(), 86.0f, ValueBox::kHeight);
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { s.levelPill.get(), s.delay.get(), s.highPass.get() }) {
				if (f != nullptr) {
					show(f, layerOn);
					if (layerOn) {
						top.place(f, 88.0f, 22.0f);
					}
				}
			}
			y += LayerTabs::kHeight + 12.0f;
		}
		else {
			show(s.kind.get(), false);
		}
		bodyBottom = y;
		if (!layerOn) {
			// AN OFF LAYER HAS A KIND AND NOTHING ELSE: no vector, no pitch, nothing to hear.
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { s.count.get(), s.driveCurve.get(), s.pitchMode.get(),
			                                                                s.velocityZone.get(), s.wires.get(), s.bells.get(),
			                                                                s.tunePill.get(), s.velocityNoise.get(), s.width.get(), s.lock.get(),
			                                                                s.vectorPad.get(), s.readout.get() }) {
				if (f != nullptr) {
					show(f, false);
				}
			}
			for (auto& f : s.field) {
				show(f.get(), false);
			}
			continue;
		}
		// THE TAB'S BODY: the terrain pad and its Lock at the left (Kit mode), everything else flowing at the right.
		float x0 = kPad;
		if (kit) {
			show(s.vectorPad.get(), true);
			s.vectorPad->setBounds(kPad, y, kVectorPadSize, kVectorPadSize);
			show(s.lock.get(), true);
			s.lock->setBounds(kPad, y + kVectorPadSize + 6.0f, 60.0f, SwitchChip::kHeight);
			x0 = kPad + kVectorPadSize + 12.0f;
		}
		else {
			show(s.vectorPad.get(), false);
			show(s.lock.get(), false);
		}
		Flow body { x0, w - kPad, x0, y, 28.0f, 6.0f };
		const auto put = [&](visage::Frame* f, bool shown, float width, float height) {
			show(f, shown);
			if (shown) {
				body.place(f, width, height);
			}
		};
		put(s.tunePill.get(), true, 92.0f, 22.0f);
		put(s.readout.get(), kit && settled.defined, 188.0f, 24.0f);
		put(s.pitchMode.get(), kit && reads.pitch, 196.0f, 22.0f);
		put(s.driveCurve.get(), uses.drive, 184.0f, 22.0f);
		put(s.count.get(), uses.count, 52.0f, ValueBox::kHeight);
		put(s.velocityNoise.get(), uses.velNoise, 92.0f, 22.0f);
		put(s.width.get(), uses.width, 92.0f, 22.0f);
		// THE SEVEN FIELD SCALERS of this layer (Kit mode: the Explore surround holds the kit's trims, and Pitched mode's own seven).
		const std::array<bool, 7> fieldShown { reads.pitch, reads.length, reads.sweep, reads.tone, reads.noise, reads.snap, reads.metal };
		for (size_t f = 0; f < s.field.size(); ++f) {
			put(s.field[f].get(), kit && fieldShown[f], 92.0f, 22.0f);
		}
		// THE ZONE ROWS (drum proposal §14): Velocity on every kind, Wires and Bells on a Snare, one to a line.
		const float zoneH = Knob::rowHeight(kZoneFace);
		float zy = body.bottom() + 6.0f;
		for (const auto& [cell, shown] : { std::pair { s.velocityZone.get(), true }, std::pair { s.wires.get(), uses.wireContour },
		                                   std::pair { s.bells.get(), uses.bell } }) {
			show(cell, shown);
			cell->setFaceDiameter(kZoneFace);
			if (shown) {
				cell->setBounds(x0, zy, w - kPad - x0, zoneH);
				zy += zoneH + 2.0f;
			}
		}
		bodyBottom = std::max(zy, kit ? y + kVectorPadSize + 6.0f + SwitchChip::kHeight : zy);
	}
	// THE PICTURE'S PLACE WHEN LAYER TWO IS OFF is said in words (draw): the content ends where the controls end.
	contentBottom_ = bodyBottom;
	redraw();
	return true;
}

void DrumPanel::resized() {
	shownPad_ = kMaxSounds; // lay out again at the new size
	sync();
}

bool DrumPanel::updateContent() {
	bool moved = sync();
	// THE SOLO CHIPS FOLLOW THE PREVIEW, which moves from the sound cards too and is no parameter any watch would carry.
	const Preview preview = bridge_.preview();
	const std::optional<size_t> solo = preview.active() ? std::optional<size_t> { preview.slot } : std::nullopt;
	if (solo != soloSeen_) {
		soloSeen_ = solo;
		for (PadSet& set : pads_) {
			set.solo->redraw();
		}
		moved = true;
	}
	// THE PAD FX RAIL, RE-ASKED EVERY TICK (the house habit), its cards advanced, its add menu opened once a fresh lane's
	// column exists.
	moved = followLane() || moved;
	moved = lane_->updateCards() > 0 || moved;
	if (pendingAddMenu_ && lane_->column() != nullptr) {
		pendingAddMenu_ = false;
		lane_->column()->showAddMenu();
		moved = true;
	}
	const bool lit = updatePulses(static_cast<int64_t>(visage::time::milliseconds()));
	return moved || lit;
}

void DrumPanel::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, std::max(0.0f, width() - railWidthNow()), height()); // the rail paints its own
	if (!kitMode()) {
		return;
	}
	const size_t selected = std::min(selection_.slot(), kMaxSounds - 1);
	if (!shownLayerOn_) {
		// AN OFF LAYER HAS NO PICTURE: said in words where the picture would be.
		canvas.setColor(kVoid);
		canvas.roundedRectangle(kPad, kPad, std::max(0.0f, width() - railWidthNow()) - 2.0f * kPad, kPictureHeight, 6.0f);
		canvas.setColor(kMuted);
		canvas.text("Layer 2 is off. Choose its Kind to strike a second hit with this pad.", font(kControlSize), visage::Font::kCenter,
		            kPad, kPad, std::max(0.0f, width() - railWidthNow()) - 2.0f * kPad, kPictureHeight);
	}
	else {
		// THE LAYER'S VECTOR, in numbers beside its Lock under the terrain pad.
		const LayerSet& L = pads_[selected].layer[layerShown_];
		if (L.vectorPad != nullptr && L.vectorPad->isVisible()) {
			char text[48] = {};
			std::snprintf(text, sizeof(text), "x %+.2f   y %+.2f", L.offsetXControl->value(), L.offsetYControl->value());
			canvas.setColor(kMuted);
			canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, L.lock->right() + 8.0f, L.lock->y(), 110.0f, L.lock->height());
		}
	}
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		const visage::Bounds b = padBounds(pad);
		const uint32_t hue = padHue(pad);
		const bool muted = std::lround(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Mute)))) != 0;
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 6.0f);
		// THE HIT PULSE: the pad's sound hue washed over it, as bright as the hit, falling over kPulseMs.
		if (const float lit = pulseOf(pad); lit > 0.0f) {
			canvas.setColor(alphaOf(hue, 0.45f * lit));
			canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 6.0f);
		}
		// THE PAD WEARS ITS SOUND'S COLOUR (pad <-> sound at a glance): the border in the hue the sound card's dot
		// and the star wear, brighter when selected.
		canvas.setColor(pad == selected ? hue : alphaOf(hue, 0.45f));
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 6.0f, pad == selected ? 2.0f : 1.0f);
		canvas.setColor(alphaOf(hue, muted ? 0.35f : 1.0f));
		canvas.circle(b.x() + 6.0f, b.y() + 7.0f, 6.0f);
		canvas.setColor(muted ? kMuted : kInk);
		const auto kindIndex = std::min(static_cast<size_t>(kindOf(pad)), kDrumKindNames.size() - 1);
		const std::string name = std::string(1, static_cast<char>('A' + pad)) + " " + kDrumKindNames[kindIndex];
		canvas.text(name, font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y() + 4.0f, b.width(), 14.0f);
		canvas.setColor(kMuted);
		const auto note = std::lround(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Note))));
		const std::string sub = "note " + std::to_string(note) + (muted ? "  ·  muted" : "");
		canvas.text(sub, font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y() + 22.0f, b.width(), 14.0f);
		const size_t choke = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Choke))),
		                                  kDrumChokeNames.size());
		if (choke != 0) {
			canvas.setColor(kModulation);
			canvas.text(std::string("choke ") + kDrumChokeNames[choke], font(kKnobLabelSize), visage::Font::kCenter, b.x(),
			            b.y() + b.height() - 14.0f, b.width(), 12.0f);
		}
		// THE FX DOT (drum proposal §17 P3): this pad's lane holds a device, so its hits go through it.
		if (padHasFx(pad)) {
			canvas.setColor(kModulation);
			canvas.text("FX", font(kKnobLabelSize), visage::Font::kLeft, b.x() + 5.0f, b.y() + b.height() - 14.0f, 20.0f, 12.0f);
		}
	}
}

uint32_t DrumPanel::padHue(size_t pad) const {
	return kSeedHues[hueIndexOf(bridge_.soundHues(), std::min(pad, kMaxSounds - 1))];
}

bool DrumPanel::updatePulses(int64_t nowMs) {
	const PadHits hits = bridge_.padHits();
	bool lit = false;
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (hits.count[pad] != pulseSeen_[pad]) {
			pulseSeen_[pad] = hits.count[pad];
			pulseLevel_[pad] = std::clamp(hits.velocity[pad], 0.0f, 1.0f);
			pulseAt_[pad] = nowMs;
		}
		lit = lit || pulseLevel_[pad] > 0.0f;
	}
	pulseNow_ = nowMs;
	if (lit) {
		redraw();
	}
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (static_cast<float>(nowMs - pulseAt_[pad]) >= kPulseMs) {
			pulseLevel_[pad] = 0.0f; // spent: no more frames owed for it
		}
	}
	return lit;
}

float DrumPanel::pulseOf(size_t pad) const {
	const float age = static_cast<float>(pulseNow_ - pulseAt_[pad]);
	if (pulseLevel_[pad] <= 0.0f || age >= kPulseMs) {
		return 0.0f;
	}
	if (reducedMotionRequested()) {
		return pulseLevel_[pad]; // lit, still, then out
	}
	return pulseLevel_[pad] * std::exp(-3.0f * age / kPulseMs);
}

void DrumPanel::clickPadForTest(size_t pad) {
	if (kitMode() && pad < kMaxSounds) {
		selection_.select(pad);
		sync();
	}
}

visage::Frame* DrumPanel::controlForTest(size_t pad, DrumControl c, size_t layer) const {
	const PadSet& set = pads_[pad];
	const LayerSet& L = set.layer[std::min<size_t>(layer, 1)];
	switch (c) {
	// THE PAD'S OWN.
	case DrumControl::Note: return set.note.get();
	case DrumControl::Choke: return set.choke.get();
	case DrumControl::Voices: return set.voices.get();
	case DrumControl::Trigger: return set.trigger.get();
	case DrumControl::Pan: return set.pan.get();
	case DrumControl::Release: return set.release.get();
	case DrumControl::Bend: return set.bend.get();
	// THE LAYER'S: the kind box (layer one's is the pad's Kind), and every control a hit reads, each layer's own copy.
	case DrumControl::Kind: return set.layer[0].kind.get();
	case DrumControl::LayerKind: return set.layer[1].kind.get();
	case DrumControl::LayerLevel: return set.layer[1].levelPill.get();
	case DrumControl::LayerDelay: return set.layer[1].delay.get();
	case DrumControl::LayerHighPass: return set.layer[1].highPass.get();
	case DrumControl::Count: return L.count.get();
	case DrumControl::Tune: return L.tunePill.get();
	case DrumControl::DriveCurve: return L.driveCurve.get();
	case DrumControl::PitchMode: return L.pitchMode.get();
	case DrumControl::VelocityNoise: return L.velocityNoise.get();
	case DrumControl::Width: return L.width.get();
	case DrumControl::Lock: return L.lock.get();
	case DrumControl::OffsetX:
	case DrumControl::OffsetY: return L.vectorPad.get();
	case DrumControl::VelocityZone: return L.velocityZone.get();
	case DrumControl::Wires: return L.wires.get();
	case DrumControl::Bells: return L.bells.get();
	case DrumControl::FieldPitch: return L.field[0].get();
	case DrumControl::FieldLength: return L.field[1].get();
	case DrumControl::FieldSweep: return L.field[2].get();
	case DrumControl::FieldTone: return L.field[3].get();
	case DrumControl::FieldNoise: return L.field[4].get();
	case DrumControl::FieldSnap: return L.field[5].get();
	case DrumControl::FieldMetal: return L.field[6].get();
	default: return nullptr;
	}
}

visage::Frame* DrumPanel::fieldPillForTest(size_t pad, size_t layer, size_t field) const {
	return pads_[pad].layer[std::min<size_t>(layer, 1)].field[std::min<size_t>(field, 6)].get();
}

bool DrumPanel::drawnForTest(size_t pad, DrumControl c, size_t layer) const {
	// A frame is drawn when it is visible AND its pad is the shown one (a hidden pad's frames are hidden too, but the pad's kit
	// structure answers for itself).
	if (visage::Frame* f = controlForTest(pad, c, layer); f != nullptr) {
		return f->isVisible();
	}
	const bool selected = shownPad() == pad && (kitMode() ? layerShown_ : 0) == layer;
	using H = HitPicture::Handle;
	switch (c) {
	// THE PAD'S LEVEL is its sound card's, and layer one's Level handle on the picture.
	case DrumControl::Level: return selected && picture_.handleShown(H::Level);
	case DrumControl::SweepTime: return selected && picture_.handleShown(H::SweepTime);
	case DrumControl::Decay: return selected && picture_.handleShown(H::Decay);
	case DrumControl::Drive: return selected && picture_.handleShown(H::Drive);
	case DrumControl::WireDecay: return selected && picture_.handleShown(H::WireDecay);
	case DrumControl::ClickDecay: return selected && picture_.handleShown(H::ClickDecay);
	case DrumControl::BellStart: return selected && picture_.handleShown(H::BellStart);
	case DrumControl::BellEnd:
	case DrumControl::BellTime: return selected && picture_.handleShown(H::BellEnd);
	case DrumControl::Bell2Hz:
		// THE SECOND BELL'S HZ IS REACHABLE ONLY WHILE FREE (the Bells zone's Knock); linked, its ratio is the place's Metal.
		return selected && picture_.handleShown(H::Bell2) && !picture_.lawsForTest().bell2Linked;
	case DrumControl::Bell2Time: return selected && picture_.handleShown(H::Bell2);
	case DrumControl::DriveTone: return selected && picture_.handleShown(H::Drive);
	case DrumControl::StartPhase: return selected && picture_.handleShown(H::StartPhase);
	case DrumControl::Spread: return selected && picture_.handleShown(H::Spread);
	case DrumControl::ClapQ:
	case DrumControl::ClapHighPass: return selected && picture_.handleShown(H::ClapBand);
	case DrumControl::KickHold: return selected && picture_.handleShown(H::Hold);
	case DrumControl::KickDip:
	case DrumControl::KickDipTime: return selected && picture_.handleShown(H::Dip);
	case DrumControl::ClickSweep:
	case DrumControl::ClickSweepTime: return selected && picture_.handleShown(H::ClickSweep);
	default: return false;
	}
}

} // namespace feathers::ui

namespace feathers::ui {

// ---- THE PAD FX RAIL (drum proposal §17 P3) ----------------------------------------------------------------------------------

float DrumPanel::railWidthNow() const {
	return kitMode() ? lane_->railWidth() : 0.0f;
}

visage::Point DrumPanel::minimumSize() const {
	visage::Point p { kMinimumWidth + railWidthNow(), kMinimumHeight };
	if (kitMode() && !lane_->collapsed()) {
		p.y = std::max(p.y, lane_->openMinimumHeight());
	}
	return p;
}

void DrumPanel::setPresentedOn(SurfaceLayers* host) { lane_->setPresentedOn(host); }

bool DrumPanel::padHasFx(size_t pad) const {
	const rack::InnerChain* lane = bridge_.drumPadLane(pad);
	return lane != nullptr && lane->bridge() != nullptr && lane->bridge()->chainLength(0) > 0;
}

bool DrumPanel::followLane() {
	const bool kit = kitMode();
	const size_t pad = std::min(selection_.slot(), kMaxSounds - 1);
	rack::InnerChain* chain = kit ? bridge_.drumPadLane(pad) : nullptr;
	bool moved = lane_->follow(chain);
	// A PHANTOM EXACTLY WHILE THE FOCUSED PAD HAS NO LANE: its (+) builds one and opens its add menu once its column exists.
	const bool wantPhantom = kit && chain == nullptr;
	if (wantPhantom != phantomShown_) {
		phantomShown_ = wantPhantom;
		if (wantPhantom) {
			lane_->setPhantom(kPadLanePasses, [this] {
				if (bridge_.drumPadLane(std::min(selection_.slot(), kMaxSounds - 1), true) != nullptr) {
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

} // namespace feathers::ui
