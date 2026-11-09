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
#include <cmath>
#include <numbers>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kPad = 12.0f;
constexpr float kPictureHeight = 230.0f;
constexpr float kRowHeight = 30.0f;
constexpr float kPadCellWidth = 72.0f;
constexpr float kPadCellHeight = 48.0f;
constexpr float kPadGap = 6.0f;
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
/// The Drive handle's floor: at Drive 0 it stands this share of the envelope up, clear of the Decay handle's line by more
/// than the hit slop.
constexpr float kDriveFloor = 0.25f;
/// The Hold handle stands this far under the level line, clear of the Level handle by more than the hit slop.
constexpr float kHoldDrop = 16.0f;
/// The zone cells' face in the kit window's list rows.
constexpr float kZoneFace = 22.0f;

} // namespace

HitPicture::HitPicture() {
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { drag(e.position); };
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

double HitPicture::naturalSpan() const {
	// A LITTLE PAST WHERE THE BODY HAS FALLEN 60 dB, so the Decay handle sits inside the plot; at least the sweep's landing.
	const double sweepEnd = laws_.sweepOctaves > 0.0 ? 4.0 * laws_.sweepTau : 0.0;
	// UP TO THE LONGEST DECAY A PAD CAN HAVE (Decay at 10 s, Length ×4: 40 s), so a long hit's handle stands inside the plot
	// rather than pinned to its edge (audition bug a, 2026-11-06: it was 6 s, and a longer Decay could only be dragged SHORTER).
	return std::clamp(std::max(1.35 * laws_.t60, sweepEnd), 0.03, 60.0);
}

float HitPicture::xOf(double seconds) const {
	// SQUARE-ROOT TIME: a hit's attack and sweep happen in its first few percent, so the axis gives the start room — a 40 ms
	// sweep in a 1.4 s view takes a sixth of the width rather than a thirtieth.
	const double u = std::sqrt(std::clamp(seconds / span(), 0.0, 1.0));
	return plotLeft() + static_cast<float>(u) * (plotRight() - plotLeft());
}

double HitPicture::secondsAt(float x) const {
	const double u = std::clamp(static_cast<double>((x - plotLeft()) / (plotRight() - plotLeft())), 0.0, 1.0);
	return u * u * span();
}

double HitPicture::dragSeconds(float x) const {
	// A TIME HANDLE REACHES ITS WHOLE RANGE (audition bug c, 2026-11-06): inside the plot, the time under the hand; past its right
	// edge, the view's span doubled every kDragOctavePixels further, so a short Decay can be dragged all the way out in one
	// gesture (the axis is frozen while it is held, and a drag used to stop at the edge: 1.35 × the T60 it started at).
	if (x <= plotRight()) {
		return secondsAt(x);
	}
	return span() * std::exp2(static_cast<double>(x - plotRight()) / kDragOctavePixels);
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

HitPicture::Handle HitPicture::handleUnder(visage::Point p) const {
	Handle held = Handle::kCount;
	float best = kHitSlop;
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (!handleShown(h)) {
			continue;
		}
		const visage::Point at = handleAt(h);
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
	held_ = handleUnder(p);
	if (held_ == Handle::kCount) {
		return;
	}
	ParamControl* c = controlOf(held_);
	frozenSpan_ = naturalSpan();
	dragging_ = true;
	pressAt_ = p;
	startValue_ = c->value();
	startX_ = handleAt(held_).x;
	startY_ = handleAt(held_).y;
	startMultiplier_ = decaySeconds_ > 0.0 ? laws_.t60 / decaySeconds_ : 1.0;
	if (held_ == Handle::WireDecay) {
		startMultiplier_ = laws_.wireT60 / std::exp2(startValue_); // Length's multiplier on the wires
	}
	if (ParamControl* second = secondOf(held_); second != nullptr) {
		startSecond_ = second->value();
		second->beginGesture();
	}
	c->beginGesture();
}

void HitPicture::drag(visage::Point p) {
	if (!dragging_ || held_ == Handle::kCount) {
		return;
	}
	ParamControl* c = controlOf(held_);
	ParamControl* second = secondOf(held_);
	const float dx = p.x - pressAt_.x;
	const float dy = p.y - pressAt_.y;
	const float envH = envBottom() - envTop();
	switch (held_) {
	case Handle::Tune: {
		// SEMITONES BY THE PLOT'S OWN SCALE: a drag of one octave's height is twelve.
		const float perOctave = (specBottom() - specTop()) / static_cast<float>(std::log2(kHighHz / kLowHz));
		c->setValue(startValue_ - 12.0 * static_cast<double>(dy / perOctave));
		break;
	}
	case Handle::SweepTime:
		// THE LANDING WHERE THE HAND IS: Sweep Time = the time under it, in log2 seconds (the control's unit).
		c->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx))));
		break;
	case Handle::Decay:
		// THE T60 WHERE THE HAND IS; Decay is that over Length's multiplier (T60 / Decay at the press), in log2 seconds.
		c->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx)) / startMultiplier_));
		break;
	case Handle::Level: c->setValue(startValue_ - 66.0 * static_cast<double>(dy / envH)); break;
	case Handle::Drive:
		// UP AND DOWN, HOW HARD; ACROSS, THE TONE BEFORE IT (§14 lever 3): half the plot's width is the tone's whole travel.
		c->setValue(startValue_ - static_cast<double>(dy / ((1.0f - kDriveFloor) * envH)));
		if (second != nullptr) {
			second->setValue(startSecond_ + static_cast<double>(dx / (0.5f * (plotRight() - plotLeft()))));
		}
		break;
	case Handle::WireDecay: c->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx)) / startMultiplier_)); break;
	case Handle::ClickDecay: c->setValue(std::log2(std::max(1.0e-4, dragSeconds(startX_ + dx)))); break;
	case Handle::BellStart: c->setValue(std::log2(hzAt(startY_ + dy))); break;
	case Handle::BellEnd:
		// UP AND DOWN, WHERE IT LANDS; ACROSS, WHEN (Bell Time: 95% of the glide).
		c->setValue(std::log2(hzAt(startY_ + dy)));
		if (second != nullptr) {
			second->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx))));
		}
		break;
	case Handle::Bell2:
		// ACROSS, HOW LONG IT RINGS; UP AND DOWN, WHERE IT RESTS while Free (Linked, the place's Metal sets its ratio).
		c->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx))));
		if (second != nullptr) {
			second->setValue(std::log2(hzAt(startY_ + dy)));
		}
		break;
	case Handle::Hold: c->setValue(1000.0 * dragSeconds(startX_ + dx)); break; // ms
	case Handle::Dip: {
		// ACROSS, WHEN (ms); UP AND DOWN, HOW DEEP: the depth whose level, below the peak, is where the hand is.
		if (second != nullptr) {
			second->setValue(1000.0 * dragSeconds(startX_ + dx));
		}
		const double db = static_cast<double>((envBottom() - (startY_ + dy)) / envH) * 66.0 - 60.0;
		c->setValue(1.0 - std::pow(10.0, std::min(0.0, db - levelDb_) / 20.0));
		break;
	}
	case Handle::ClickSweep:
		// UP AND DOWN, HOW FAR ABOVE THE SWEEP'S TOP IT STARTS (octaves); ACROSS, ITS TIME.
		c->setValue(std::log2(hzAt(startY_ + dy) / laws_.hz) - laws_.sweepOctaves);
		if (second != nullptr) {
			second->setValue(std::log2(std::max(1.0e-5, dragSeconds(startX_ + dx))));
		}
		break;
	case Handle::StartPhase:
		// UP AND DOWN, DEGREES: the envelope's height is the phase's whole 90.
		c->setValue(startValue_ - 90.0 * static_cast<double>(dy / envH));
		break;
	case Handle::Spread:
		// ACROSS, WHEN THE LAST HAND LANDS (Spread, in log2 seconds).
		c->setValue(std::log2(std::max(1.0e-3, dragSeconds(startX_ + dx))));
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
	if (dragging_ && held_ != Handle::kCount) {
		controlOf(held_)->endGesture();
		if (ParamControl* second = secondOf(held_); second != nullptr) {
			second->endGesture();
		}
	}
	dragging_ = false;
	held_ = Handle::kCount;
}

void HitPicture::dragForTest(Handle h, float dx, float dy) {
	const visage::Point at = handleAt(h);
	press(at);
	drag({ at.x + dx, at.y + dy });
	release();
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

	constexpr int kSteps = 64;
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
		canvas.setColor(alphaOf(kValue, 0.35f));
		canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, top, w / kSteps + 0.5f, envBottom() - top);
		canvas.setColor(kValue);
		canvas.fill(plotLeft() + w * static_cast<float>(i) / kSteps, top - 1.0f, w / kSteps + 0.5f, 2.0f);
	}

	// THE HANDLES, each with its readout.
	const std::array<visage::Bounds, kHandleCount> labels = labelRects();
	for (size_t i = 0; i < kHandleCount; ++i) {
		const auto h = static_cast<Handle>(i);
		if (!handleShown(h)) {
			continue;
		}
		const visage::Point at = handleAt(h);
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
	// COLLISION-AVOIDING PLACEMENT: each shown handle's label takes the first of eight places around it that stays inside the
	// plot and clear of every label placed before it and of every handle; with none clear, the first that stays inside.
	std::array<visage::Bounds, kHandleCount> out {};
	std::vector<visage::Bounds> placed;
	std::vector<visage::Bounds> dots;
	for (size_t i = 0; i < kHandleCount; ++i) {
		if (handleShown(static_cast<Handle>(i))) {
			const visage::Point at = handleAt(static_cast<Handle>(i));
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
		const visage::Point at = handleAt(handle);
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

//-----------//
// DrumPanel //
//-----------//

DrumPanel::DrumPanel(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection, Notifier* notifier)
    : bridge_(bridge), selection_(selection) {
	mode_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::DrumMode), kDrumModeNames, notifier);
	pitchedKindControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumPitchedKind));
	pitchedKind_ = std::make_unique<ValueBox>(*pitchedKindControl_, "Kind");
	seedControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumSeed));
	seed_ = std::make_unique<ValueBox>(*seedControl_, "Seed");
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumPitchedKind), pitchedKind_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumSeed), seed_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this));
	addChild(&picture_);
	addChild(mode_.get());
	addChild(pitchedKind_.get());
	addChild(seed_.get());
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		const auto box = [&](std::unique_ptr<ParamControl>& control, std::unique_ptr<ValueBox>& out, DrumControl c,
		                     const char* label) {
			const ParamDesc& d = descOf(drumParam(slot, c));
			control = std::make_unique<ParamControl>(bridge, &d);
			out = std::make_unique<ValueBox>(*control, label);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), out.get()));
			addChild(out.get());
		};
		box(set.kindControl, set.kind, DrumControl::Kind, "Kind");
		box(set.noteControl, set.note, DrumControl::Note, "Note");
		box(set.chokeControl, set.choke, DrumControl::Choke, "Choke");
		box(set.countControl, set.count, DrumControl::Count, "Claps");
		set.voices = std::make_unique<SegmentedControl>(bridge, registry, descOf(drumParam(slot, DrumControl::Voices)),
		                                                kDrumVoicesNames, notifier);
		set.trigger = std::make_unique<SegmentedControl>(bridge, registry, descOf(drumParam(slot, DrumControl::Trigger)),
		                                                 kDrumTriggerNames, notifier);
		addChild(set.voices.get());
		addChild(set.trigger.get());
		// THE HANDLES' CONTROLS: the picture writes them for the selected pad, and watches every pad's, so a host move
		// redraws it and each of them is reached by a hand (the reachability guard asks the registry).
		const auto handle = [&](std::unique_ptr<ParamControl>& control, DrumControl c) {
			const ParamDesc& d = descOf(drumParam(slot, c));
			control = std::make_unique<ParamControl>(bridge, &d);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), &picture_));
		};
		handle(set.tune, DrumControl::Tune);
		handle(set.sweepTime, DrumControl::SweepTime);
		handle(set.decay, DrumControl::Decay);
		handle(set.level, DrumControl::Level);
		handle(set.drive, DrumControl::Drive);
		handle(set.driveTone, DrumControl::DriveTone);
		handle(set.wireDecay, DrumControl::WireDecay);
		handle(set.clickDecay, DrumControl::ClickDecay);
		handle(set.bellStart, DrumControl::BellStart);
		handle(set.bellEnd, DrumControl::BellEnd);
		handle(set.bellTime, DrumControl::BellTime);
		handle(set.bell2Hz, DrumControl::Bell2Hz);
		handle(set.bell2Time, DrumControl::Bell2Time);
		handle(set.hold, DrumControl::KickHold);
		handle(set.dip, DrumControl::KickDip);
		handle(set.dipTime, DrumControl::KickDipTime);
		handle(set.clickSweep, DrumControl::ClickSweep);
		handle(set.clickSweepTime, DrumControl::ClickSweepTime);
		handle(set.startPhase, DrumControl::StartPhase);
		handle(set.spread, DrumControl::Spread);
		handle(set.clapQ, DrumControl::ClapQ);
		handle(set.clapHighPass, DrumControl::ClapHighPass);
		const auto pill = [&](std::unique_ptr<ParamControl>& control, std::unique_ptr<FaderPill>& out, DrumControl c,
		                      const char* caption) {
			const ParamDesc& d = descOf(drumParam(slot, c));
			control = std::make_unique<ParamControl>(bridge, &d);
			out = std::make_unique<FaderPill>(*control, caption);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), out.get()));
			addChild(out.get());
		};
		pill(set.levelMirrorControl, set.levelMirror, DrumControl::Level, "Level");
		pill(set.tuneMirrorControl, set.tuneMirror, DrumControl::Tune, "Tune");
		pill(set.panControl, set.pan, DrumControl::Pan, "Pan");
		pill(set.releaseControl, set.release, DrumControl::Release, "Release");
		pill(set.velocityNoiseControl, set.velocityNoise, DrumControl::VelocityNoise, "Vel Noise");
		pill(set.widthControl, set.width, DrumControl::Width, "Width");
		pill(set.bendControl, set.bend, DrumControl::Bend, "Bend");
		// THE LAYER ROW (stage (c)).
		box(set.layerKindControl, set.layerKind, DrumControl::LayerKind, "Layer");
		pill(set.layerLevelControl, set.layerLevel, DrumControl::LayerLevel, "Layer Lvl");
		pill(set.layerDelayControl, set.layerDelay, DrumControl::LayerDelay, "Delay");
		pill(set.layerHighPassControl, set.layerHighPass, DrumControl::LayerHighPass, "Layer HP");
		pill(set.layerOffsetXControl, set.layerOffsetX, DrumControl::LayerOffsetX, "Offset X");
		pill(set.layerOffsetYControl, set.layerOffsetY, DrumControl::LayerOffsetY, "Offset Y");
		{
			const ParamDesc& d = descOf(drumParam(slot, DrumControl::LayerLock));
			set.layerLockControl = std::make_unique<ParamControl>(bridge, &d);
			set.layerLock = std::make_unique<SwitchChip>(*set.layerLockControl, "Lock");
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), set.layerLock.get()));
			addChild(set.layerLock.get());
		}
		// THE ZONE CONTROLS (drum proposal §14): one coordinate each, its zone named in a dropdown, in the list row's layout.
		const auto zone = [&](std::unique_ptr<ParamControl>& control, std::unique_ptr<ZoneCoordCell>& out, DrumControl c,
		                      const char* caption, auto names) {
			const ParamDesc& d = descOf(drumParam(slot, c));
			control = std::make_unique<ParamControl>(bridge, &d);
			std::vector<ZoneSelect::ZoneName> zoneNames;
			for (const char* n : names) {
				zoneNames.push_back({ n, "" });
			}
			out = std::make_unique<ZoneCoordCell>(*control, caption, std::move(zoneNames), ZoneSelect::kCoordinatePerLap);
			out->setRowLayout(true);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), out.get()));
			addChild(out.get());
		};
		zone(set.velocityZoneControl, set.velocityZone, DrumControl::VelocityZone, "Velocity", drum::kVelocityZoneNames);
		zone(set.wiresControl, set.wires, DrumControl::Wires, "Wires", drum::kWiresZoneNames);
		zone(set.bellsControl, set.bells, DrumControl::Bells, "Bells", drum::kBellsZoneNames);
		set.driveCurve = std::make_unique<SegmentedControl>(bridge, registry, descOf(drumParam(slot, DrumControl::DriveCurve)),
		                                                    kDrumDriveCurveNames, notifier);
		addChild(set.driveCurve.get());
		// The Bells zone decides whether the second bell is linked, which is what its handle writes, so a move re-lays.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Bells)), this));
		// The pad's kind and Trigger decide what is drawn, so a move of either re-lays the panel.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Kind)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Trigger)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::LayerKind)), this));
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
	for (PadSet& set : pads_) {
		for (ParamControl* c :
		     { set.tune.get(), set.sweepTime.get(), set.decay.get(), set.level.get(), set.drive.get(), set.driveTone.get(),
		       set.wireDecay.get(), set.clickDecay.get(), set.bellStart.get(), set.bellEnd.get(), set.bellTime.get(),
		       set.bell2Hz.get(), set.bell2Time.get(), set.hold.get(), set.dip.get(), set.dipTime.get(), set.clickSweep.get(),
		       set.clickSweepTime.get(), set.startPhase.get(), set.spread.get(), set.clapQ.get(), set.clapHighPass.get(),
		       set.velocityZoneControl.get(), set.wiresControl.get(), set.bellsControl.get(), set.levelMirrorControl.get(),
		       set.tuneMirrorControl.get(), set.panControl.get(), set.releaseControl.get(), set.velocityNoiseControl.get(),
		       set.widthControl.get(), set.bendControl.get(), set.layerLevelControl.get(), set.layerDelayControl.get(),
		       set.layerHighPassControl.get(), set.layerOffsetXControl.get(), set.layerOffsetYControl.get() }) {
			c->setModulation(mod, history);
		}
	}
}

bool DrumPanel::kitMode() const {
	return steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	       == static_cast<size_t>(DrumMode::Kit);
}

drum::Kind DrumPanel::kindOf(size_t pad) const {
	const clap_id id = kitMode() ? static_cast<clap_id>(drumParam(pad, DrumControl::Kind))
	                             : static_cast<clap_id>(ParamId::DrumPitchedKind);
	return static_cast<drum::Kind>(steppedIndex(bridge_.value(id), kDrumKindNames.size()));
}

drum::Fields DrumPanel::fieldsOf(size_t pad) const {
	// THE PAD WHERE IT REALLY STANDS (the honesty rule: what you see is what the pad plays): the character the synth builds, by
	// WaveDisplay's road — modulated values, the played travel (drift and pulls), the planes, residue, gamma and the likes'
	// warp — then the bank's own build of the pads (buildDrumFor). Measured cost: seven triangle reads a sound.
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
	const DrumCharacter d = buildDrumFor(c);
	return d.pads[d.kit ? pad : 0];
}

drum::Laws DrumPanel::lawsOf(size_t pad) const {
	// THE VOICE'S MAPPING (OscVoice::controlDrum): the pad's controls over its real fields, at velocity 0.8. In Kit mode the
	// kind's resting pitch at Tune; in Pitched mode a C3 at Tune.
	const auto v = [&](DrumControl c) { return bridge_.modulated(static_cast<clap_id>(drumParam(pad, c))); };
	drum::Controls c;
	c.decaySeconds = std::exp2(v(DrumControl::Decay));
	c.sweepSeconds = std::exp2(v(DrumControl::SweepTime));
	c.drive = v(DrumControl::Drive);
	c.count = static_cast<int>(std::lround(v(DrumControl::Count)));
	c.spreadSeconds = std::exp2(v(DrumControl::Spread));
	c.pitchRangeSemitones = kitMode() ? 3.0 : 0.5;
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
	const drum::Kind k = kindOf(pad);
	const double base = (kitMode() ? drum::kindBaseHz(k) : 130.8127826502993) * std::exp2(v(DrumControl::Tune) / 12.0);
	return drum::lawsFor(k, fieldsOf(pad), c, base, 0.8);
}

visage::Bounds DrumPanel::padBounds(size_t pad) const {
	// ONE ROW OF EIGHT, A to H left to right (the kit window's layout, by design): the panel is exactly as wide as
	// the row, and the pad's controls sit under it.
	const size_t col = pad % kMaxSounds;
	const float top = kPad + kPictureHeight + kPad + kRowHeight + kPad;
	return { kPad + static_cast<float>(col) * (kPadCellWidth + kPadGap), top,
		     kPadCellWidth, kPadCellHeight };
}

bool DrumPanel::sync() {
	const bool kit = kitMode();
	const size_t pad = kit ? std::min(selection_.slot(), kMaxSounds - 1) : 0;
	const drum::Kind kind = kindOf(pad);
	const drum::ControlUse uses = drum::kindControls(kind);
	const bool gate = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Trigger))),
	                               kDrumTriggerNames.size())
	                  == 1;
	const bool layered = kit && steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::LayerKind))),
	                                         kDrumLayerKindNames.size())
	                                    > 0;
	PadSet& mine = pads_[pad];
	HitPicture::Controls handles;
	handles.tune = mine.tune.get();
	handles.decay = mine.decay.get();
	handles.level = mine.level.get();
	handles.sweepTime = uses.sweepTime ? mine.sweepTime.get() : nullptr;
	handles.drive = uses.drive ? mine.drive.get() : nullptr;
	handles.driveTone = uses.drive ? mine.driveTone.get() : nullptr;
	if (uses.snareEnvelopes) {
		handles.wireDecay = mine.wireDecay.get();
		handles.clickDecay = mine.clickDecay.get();
	}
	if (uses.bell) {
		handles.bellStart = mine.bellStart.get();
		handles.bellEnd = mine.bellEnd.get();
		handles.bellTime = mine.bellTime.get();
		handles.bell2 = mine.bell2Hz.get();
		handles.bell2Time = mine.bell2Time.get();
	}
	if (uses.kickShape) {
		handles.hold = mine.hold.get();
		handles.dip = mine.dip.get();
		handles.dipTime = mine.dipTime.get();
		handles.clickSweep = mine.clickSweep.get();
		handles.clickSweepTime = mine.clickSweepTime.get();
		handles.startPhase = mine.startPhase.get();
	}
	if (uses.spread) {
		handles.spread = mine.spread.get();
	}
	if (uses.clapFilter) {
		handles.clapHighPass = mine.clapHighPass.get();
		handles.clapQ = mine.clapQ.get();
	}
	picture_.show(kind, lawsOf(pad), std::exp2(bridge_.modulated(static_cast<clap_id>(drumParam(pad, DrumControl::Decay)))),
	              bridge_.modulated(static_cast<clap_id>(drumParam(pad, DrumControl::Level))), handles);
	if (pad == shownPad_ && kit == shownKit_ && kind == shownKind_ && gate == shownGate_) {
		return false;
	}
	shownPad_ = pad;
	shownKit_ = kit;
	shownKind_ = kind;
	shownGate_ = gate;
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
	// THE LAYOUT: the picture with its handles across the top; under it the engine row (Mode, the Pitched Kind, Seed); then
	// the pads at the left, and the pad's own boxes and pills at the right.
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
	// THE CONTROLS UNDER THE PADS, across the whole width; in Pitched mode (no pads) where
	// the pads would be.
	const float controlsX = kPad;
	const float boxY = kit ? padBounds(0).bottom() + kPad : rowY + kRowHeight + kPad;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		const bool own = slot == pad;
		float x = controlsX;
		const auto seatBox = [&](auto* f, bool shown, float width) {
			show(f, own && shown);
			f->setBounds(x, boxY, width, ValueBox::kHeight);
			if (own && shown) {
				x += width + 6.0f;
			}
		};
		seatBox(set.kind.get(), kit, 86.0f);
		seatBox(set.note.get(), kit, 52.0f);
		seatBox(set.choke.get(), kit, 52.0f);
		seatBox(set.count.get(), uses.count, 52.0f);
		const float segY = boxY + ValueBox::kHeight + 6.0f;
		show(set.voices.get(), own && kit);
		set.voices->setBounds(controlsX, segY, 96.0f, 22.0f);
		show(set.trigger.get(), own);
		set.trigger->setBounds(controlsX + (kit ? 104.0f : 0.0f), segY, 128.0f, 22.0f);
		// THE ZONE ROW (drum proposal §14): Velocity on every kind, Wires and Bells on a Snare, side by side in list rows.
		const float zoneY = segY + 30.0f;
		const float zoneH = Knob::rowHeight(kZoneFace);
		const float zoneW = std::floor((w - kPad - controlsX - 2.0f * 6.0f) / 3.0f);
		float zx = controlsX;
		for (const auto& [cell, shown] : { std::pair { set.velocityZone.get(), true }, std::pair { set.wires.get(), uses.wireContour },
		                                   std::pair { set.bells.get(), uses.bell } }) {
			show(cell, own && shown);
			cell->setFaceDiameter(kZoneFace);
			cell->setBounds(zx, zoneY, zoneW, zoneH);
			if (own && shown) {
				zx += zoneW + 6.0f;
			}
		}
		// THE PILLS, FLOWING INTO ROWS: Level and Tune (mirroring their handles), Pan, Release on a Gate pad, the drive's curve
		// (all three on its face) wherever Drive is read, Velocity Noise where Noise is read, Width on a Snare or a Clap.
		float px = controlsX;
		float pillY = zoneY + zoneH + 8.0f;
		const float pillW = 92.0f;
		int pillRow = 0;
		const auto seat = [&](auto* p, bool shown, float width) {
			show(p, own && shown);
			if (own && shown && px > controlsX && px + width > w - kPad) {
				px = controlsX;
				++pillRow;
			}
			p->setBounds(px, pillY + static_cast<float>(pillRow) * 28.0f, width, 22.0f);
			if (own && shown) {
				px += width + 6.0f;
			}
		};
		seat(set.levelMirror.get(), true, pillW);
		seat(set.tuneMirror.get(), true, pillW);
		seat(set.pan.get(), true, pillW);
		seat(set.bend.get(), kit, pillW); // the pitch bend's share (decision 8): a kit pad's alone; Pitched mode follows the voice
		seat(set.release.get(), gate, pillW);
		seat(set.driveCurve.get(), uses.drive, 2.0f * pillW);
		seat(set.velocityNoise.get(), uses.velNoise, pillW);
		seat(set.width.get(), uses.width, pillW);
		// THE LAYER ROW (stage (c)), Kit mode alone, on a line of its own under the pills: its kind, and while it is on its
		// Level, Delay, High Pass, Lock and offset vector, flowing on.
		const float layerY = pillY + static_cast<float>(pillRow + 1) * 28.0f + 4.0f;
		show(set.layerKind.get(), own && kit);
		set.layerKind->setBounds(controlsX, layerY - 4.0f, 86.0f, ValueBox::kHeight);
		px = controlsX + 92.0f;
		pillY = layerY;
		pillRow = 0;
		seat(set.layerLevel.get(), layered, pillW);
		seat(set.layerDelay.get(), layered, pillW);
		seat(set.layerHighPass.get(), layered, pillW);
		seat(set.layerLock.get(), layered, 60.0f);
		seat(set.layerOffsetX.get(), layered, pillW);
		seat(set.layerOffsetY.get(), layered, pillW);
	}
	redraw();
	return true;
}

void DrumPanel::resized() {
	shownPad_ = kMaxSounds; // lay out again at the new size
	sync();
}

bool DrumPanel::updateContent() {
	bool moved = sync();
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

visage::Frame* DrumPanel::controlForTest(size_t pad, DrumControl c) const {
	const PadSet& set = pads_[pad];
	switch (c) {
	case DrumControl::Kind: return set.kind.get();
	case DrumControl::Note: return set.note.get();
	case DrumControl::Choke: return set.choke.get();
	case DrumControl::Count: return set.count.get();
	case DrumControl::Voices: return set.voices.get();
	case DrumControl::Trigger: return set.trigger.get();
	case DrumControl::Level: return set.levelMirror.get();
	case DrumControl::Tune: return set.tuneMirror.get();
	case DrumControl::Pan: return set.pan.get();
	case DrumControl::Release: return set.release.get();
	case DrumControl::DriveCurve: return set.driveCurve.get();
	case DrumControl::VelocityNoise: return set.velocityNoise.get();
	case DrumControl::Width: return set.width.get();
	case DrumControl::Bend: return set.bend.get();
	case DrumControl::LayerKind: return set.layerKind.get();
	case DrumControl::LayerLevel: return set.layerLevel.get();
	case DrumControl::LayerDelay: return set.layerDelay.get();
	case DrumControl::LayerHighPass: return set.layerHighPass.get();
	case DrumControl::LayerLock: return set.layerLock.get();
	case DrumControl::LayerOffsetX: return set.layerOffsetX.get();
	case DrumControl::LayerOffsetY: return set.layerOffsetY.get();
	case DrumControl::VelocityZone: return set.velocityZone.get();
	case DrumControl::Wires: return set.wires.get();
	case DrumControl::Bells: return set.bells.get();
	default: return nullptr;
	}
}

bool DrumPanel::drawnForTest(size_t pad, DrumControl c) const {
	if (visage::Frame* f = controlForTest(pad, c); f != nullptr) {
		return f->isVisible();
	}
	const bool selected = (kitMode() ? std::min(selection_.slot(), kMaxSounds - 1) : 0) == pad;
	using H = HitPicture::Handle;
	switch (c) {
	case DrumControl::Tune: return selected && picture_.handleShown(H::Tune);
	case DrumControl::SweepTime: return selected && picture_.handleShown(H::SweepTime);
	case DrumControl::Decay: return selected && picture_.handleShown(H::Decay);
	case DrumControl::Level: return selected && picture_.handleShown(H::Level);
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
