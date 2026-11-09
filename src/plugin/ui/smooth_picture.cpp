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

#include "plugin/ui/smooth_picture.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/zone_select.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace sm = fx::smooth;

namespace {

constexpr float kCaptionSize = 8.0f;
constexpr float kCaptionHeight = 11.0f;

/// THE ROUTE DRAG'S RULE — OWLTT's graph and the knob: a drag with a source armed edits its amount, unless Alt
/// asks for the value.
bool routeGestureWanted(const device::Modulatable& ctrl, const visage::MouseEvent& e) {
	return !e.isAltDown() && ctrl.display().routable;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE MODEL                                                                                         //
//--------------------------------------------------------------------------------------------------//

bool SmoothPictureModel::refresh(const sm::Settings& now) {
	if (have && now == drawn) {
		return false;
	}
	drawn = now;
	have = true;
	picture = sm::pictureOf(now);
	float peak = 0.0f;
	for (int i = 0; i < kCurvePoints; ++i) {
		curve[static_cast<size_t>(i)] = sm::transferAt(picture, inputAt(i));
		peak = std::max(peak, std::fabs(curve[static_cast<size_t>(i)]));
	}
	outputSpan = std::max(1.0f, peak * 1.08f);
	harmonics = sm::harmonicsOf(picture, kHarmonicLevel);
	++computations;
	return true;
}

//--------------------------------------------------------------------------------------------------//
// THE CURVE                                                                                         //
//--------------------------------------------------------------------------------------------------//

SmoothCurveView::SmoothCurveView(const SmoothPictureModel& model, device::Modulatable& drive)
    : model_(model), drive_(drive) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!grabsDrive(e.position)) {
			return;
		}
		if (e.repeatClickCount() >= 2) {
			(void)drive_.resetToDefault();
			redraw();
			return;
		}
		dragging_ = true;
		if (routeGestureWanted(drive_, e)) {
			routeDragging_ = true;
			routeAnchorY_ = e.position.y;
			const float before = drive_.display().amount;
			routeAnchorAmount_ = before != 0.0f ? before : Knob::kRouteSeedAmount;
			drive_.beginAmountGesture();
			drive_.setAmount(routeAnchorAmount_);
		}
		else {
			// A PRESS WRITES NOTHING: it records where the hand is and where the ceiling is, and the drag moves
			// the ceiling by how far the hand has gone since. So a bare click neither jumps Drive to the pointer
			// (a long way when the ceiling is pinned at the edge, off-scale) nor records an undo step.
			routeDragging_ = false;
			gestureOpen_ = false;
			grabX_ = e.position.x;
			grabCeiling_ = sm::ceilingInputOf(model_.picture.zone, drive_.value());
			grabSide_ = e.position.x >= xOfInput(0.0f) ? 1.0f : -1.0f;		}
		redraw();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!dragging_) {
			return;
		}
		if (routeDragging_) {
			// UP IS MORE, the knob's route drag: a full `kRouteDragRange` of travel is one unit of amount.
			const float amount = std::clamp(routeAnchorAmount_ + (routeAnchorY_ - e.position.y) / Knob::kRouteDragRange,
			                                -device::kMaxAmount, device::kMaxAmount);
			drive_.setAmount(amount);
		}
		else {
			if (!gestureOpen_) {
				if (e.position.x == grabX_) {
					return;
				}
				drive_.beginGesture();
				gestureOpen_ = true;
			}
			dragTo(e.position.x);
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!dragging_) {
			return;
		}
		if (routeDragging_) {
			if (std::fabs(drive_.display().amount) < device::kAmountUnroutedBelow) {
				drive_.setAmount(0.0f);
			}
			drive_.endAmountGesture();
		}
		else if (gestureOpen_) {
			drive_.endGesture();
		}
		gestureOpen_ = false;
		dragging_ = false;
		routeDragging_ = false;
		redraw();
	};
	onMouseMove() = [this](const visage::MouseEvent& e) {
		const bool wasOver = hovering_ && grabsDrive(hover_);
		hover_ = e.position;
		hovering_ = true;
		if (grabsDrive(hover_) != wasOver) {
			redraw(); // only when the handle's highlight changes: moving across the plot costs nothing
		}
	};
	onMouseExit() = [this](const visage::MouseEvent&) {
		const bool wasOver = hovering_ && grabsDrive(hover_);
		hovering_ = false;
		if (wasOver) {
			redraw();
		}
	};
	// THE TOOLTIP ROAD, AFTER THE HANDLERS ABOVE: it ADDS its own (`+=`), and they ASSIGN.
	installTooltipHover(*this, *this);
}

void SmoothCurveView::setInputPeak(bool has, float peak) {
	hasPeak_ = has;
	peak_ = peak;
}

visage::Bounds SmoothCurveView::plotBounds() const {
	// ROOM UNDER THE PLOT FOR THE HANDLE'S DIAMOND AND ITS READOUT, and on the left for the panel's numbers.
	// AND HALF A HANDLE EACH SIDE, so a ceiling at the edge of the span shows its whole diamond.
	const float inset = kHandleSize * 0.5f + 1.0f;
	const float left = annotated_ ? 26.0f : inset;
	const float bottom = kHandleSize + kCaptionHeight + 4.0f;
	return { left, 2.0f, std::max(0.0f, static_cast<float>(width()) - left - inset),
		     std::max(0.0f, static_cast<float>(height()) - 2.0f - bottom) };
}

float SmoothCurveView::xOfInput(float input) const {
	const visage::Bounds b = plotBounds();
	const float t = (input + SmoothPictureModel::kInputSpan) / (2.0f * SmoothPictureModel::kInputSpan);
	return b.x() + t * b.width();
}

float SmoothCurveView::inputOfX(float x) const {
	const visage::Bounds b = plotBounds();
	const float t = b.width() > 0.0f ? (x - b.x()) / b.width() : 0.5f;
	return -SmoothPictureModel::kInputSpan + 2.0f * SmoothPictureModel::kInputSpan * t;
}

float SmoothCurveView::yOfOutput(float output) const {
	const visage::Bounds b = plotBounds();
	const float t = (output + model_.outputSpan) / (2.0f * model_.outputSpan);
	return b.bottom() - t * b.height();
}

bool SmoothCurveView::ceilingOffScale() const {
	return model_.picture.ceilingInput > SmoothPictureModel::kInputSpan;
}

float SmoothCurveView::ceilingX() const {
	return xOfInput(std::min(model_.picture.ceilingInput, SmoothPictureModel::kInputSpan));
}

bool SmoothCurveView::grabsDrive(visage::Point p) const {
	const visage::Bounds b = plotBounds();
	const float cx = ceilingX();
	const float mirror = xOfInput(-std::min(model_.picture.ceilingInput, SmoothPictureModel::kInputSpan));
	const bool inHeight = p.y >= b.y() && p.y <= static_cast<float>(height());
	return inHeight && (std::fabs(p.x - cx) <= kGrabSlop || std::fabs(p.x - mirror) <= kGrabSlop);
}

void SmoothCurveView::dragTo(float x) {
	// THE CEILING MOVES AS FAR AS THE HAND HAS, from where it was at the press — outward on either side is
	// a higher ceiling. Anchored to the STORED value, so an off-scale ceiling starts from where it really is
	// rather than from the plot's edge it is drawn pinned to.
	const float moved = (inputOfX(x) - inputOfX(grabX_)) * grabSide_;
	drive_.setValue(sm::driveKnobForCeiling(model_.picture.zone, std::max(grabCeiling_ + moved, 0.02f)));
}

void SmoothCurveView::draw(visage::Canvas& canvas) {
	const visage::Bounds b = plotBounds();
	if (b.width() <= 4.0f || b.height() <= 4.0f) {
		return;
	}
	const sm::Picture& pic = model_.picture;

	// THE FRAME, THE AXES AND THE 0 dBFS LINES (the level a full voice arrives at, and the output's).
	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(b.x(), b.y(), b.width(), b.height(), 1.0f);
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.segment(b.x(), yOfOutput(0.0f), b.right(), yOfOutput(0.0f), 1.0f, true);
	canvas.segment(xOfInput(0.0f), b.y(), xOfInput(0.0f), b.bottom(), 1.0f, true);
	canvas.setColor(withAlpha(kLine, 0.22f));
	for (const float level : { 1.0f, -1.0f }) {
		const float y = yOfOutput(level);
		if (y > b.y() + 1.0f && y < b.bottom() - 1.0f) {
			canvas.segment(b.x(), y, b.right(), y, 1.0f, true);
		}
	}

	// THE CEILING — dashed where it clips, dotted in the FM zones where it is only a reference.
	const bool overHandle = hovering_ && grabsDrive(hover_);
	const uint32_t ceilingColour = (dragging_ || overHandle) ? kValue : withAlpha(kValue, 0.75f);
	const float dash = pic.clips ? 5.0f : 2.0f;
	const float gap = pic.clips ? 3.0f : 3.0f;
	const float cRight = ceilingX();
	const float cLeft = xOfInput(-std::min(pic.ceilingInput, SmoothPictureModel::kInputSpan));
	canvas.setColor(ceilingColour);
	for (const float cx : { cLeft, cRight }) {
		for (float y = b.y(); y < b.bottom(); y += dash + gap) {
			canvas.segment(cx, y, cx, std::min(b.bottom(), y + dash), 1.4f, true);
		}
	}
	// THE MODULATED CEILING, where a route has put Drive this instant — faint, and only while it differs.
	const device::ModDisplay mod = drive_.display();
	if (mod.isDestination && std::fabs(mod.modulated - drive_.value()) > 1.0e-4) {
		const float modInput = std::min(sm::ceilingInputOf(pic.zone, static_cast<float>(mod.modulated)),
		                                SmoothPictureModel::kInputSpan);
		canvas.setColor(withAlpha(kModulation, 0.8f));
		for (const float cx : { xOfInput(-modInput), xOfInput(modInput) }) {
			canvas.segment(cx, b.y(), cx, b.bottom(), 1.0f, true);
		}
	}

	// THE CURVE ITSELF — the real core, point by point.
	canvas.setColor(withAlpha(kAccent, 0.95f));
	for (int i = 1; i < SmoothPictureModel::kCurvePoints; ++i) {
		const float x0 = xOfInput(SmoothPictureModel::inputAt(i - 1));
		const float x1 = xOfInput(SmoothPictureModel::inputAt(i));
		const float y0 = std::clamp(yOfOutput(model_.curve[static_cast<size_t>(i - 1)]), b.y(), b.bottom());
		const float y1 = std::clamp(yOfOutput(model_.curve[static_cast<size_t>(i)]), b.y(), b.bottom());
		canvas.segment(x0, y0, x1, y1, 1.8f, true);
	}

	// THE LIVE INPUT PEAK, both sides of the axis, when the bus is publishing it.
	if (hasPeak_) {
		const float px = std::min(peak_, SmoothPictureModel::kInputSpan);
		canvas.setColor(kInk);
		for (const float x : { xOfInput(-px), xOfInput(px) }) {
			canvas.triangleUp(x - 4.0f, b.bottom() - 7.0f, 8.0f);
		}
	}

	// THE HANDLE: a diamond under each ceiling line, and what it is set to beside it.
	const float hy = b.bottom() + 2.0f;
	canvas.setColor(ceilingColour);
	for (const float cx : { cLeft, cRight }) {
		canvas.diamond(cx - kHandleSize * 0.5f, hy, kHandleSize, 1.0f);
	}
	if (ceilingOffScale()) {
		canvas.triangleRight(b.right() - 6.0f, hy + 1.0f, 6.0f);
	}
	char caption[48];
	// IN dB, AS THE FADER READS — and "-inf" at the bottom, where nothing reaches the shaper.
	if (pic.driveGain > 0.0f) {
		std::snprintf(caption, sizeof caption, "DRIVE %+.1f dB", static_cast<double>(20.0f * std::log10(pic.driveGain)));
	}
	else {
		std::snprintf(caption, sizeof caption, "DRIVE -inf dB");
	}
	canvas.setColor(kMuted);
	canvas.text(caption, font(kCaptionSize), visage::Font::kLeft, b.x(), hy + kHandleSize, b.width() * 0.6f,
	            kCaptionHeight);
	canvas.text(pic.clips ? "CLIP" : "0 dB", font(kCaptionSize), visage::Font::kRight, b.x(), hy + kHandleSize,
	            b.width(), kCaptionHeight);

	if (annotated_) {
		// THE PANEL'S NUMBERS: the output's 0 dBFS and its span, on the left.
		char top[16];
		std::snprintf(top, sizeof top, "%.1f", static_cast<double>(model_.outputSpan));
		canvas.setColor(kDisabled);
		canvas.text(top, font(kCaptionSize), visage::Font::kRight, 0.0f, b.y(), b.x() - 3.0f, kCaptionHeight);
		canvas.text("0", font(kCaptionSize), visage::Font::kRight, 0.0f, yOfOutput(0.0f) - kCaptionHeight * 0.5f,
		            b.x() - 3.0f, kCaptionHeight);
		const float y1 = yOfOutput(1.0f);
		if (y1 > b.y() + kCaptionHeight) {
			canvas.text("1", font(kCaptionSize), visage::Font::kRight, 0.0f, y1 - kCaptionHeight * 0.5f, b.x() - 3.0f,
			            kCaptionHeight);
		}
	}
}

const char* SmoothCurveView::tooltipAt(visage::Point local) const {
	// **A TIP IS A CONTROL'S DECLARATION** (the tooltip sweep's rule): the ceiling IS Drive, so it says what Drive
	// says. The rest of the plot is a picture, not a control, and says nothing.
	if (grabsDrive(local)) {
		return sm::kParamDescs[static_cast<size_t>(sm::Param::Drive)].tooltip;
	}
	return nullptr;
}

//--------------------------------------------------------------------------------------------------//
// THE BARS                                                                                          //
//--------------------------------------------------------------------------------------------------//

SmoothHarmonicsView::SmoothHarmonicsView(const SmoothPictureModel& model)
    : model_(model) {
	installTooltipHover(*this, *this);
}

float SmoothHarmonicsView::fractionOfLevel(float level) {
	if (!(level > 0.0f)) {
		return 0.0f;
	}
	const float db = 20.0f * std::log10(level);
	return std::clamp((db - kFloorDb) / (kTopDb - kFloorDb), 0.0f, 1.0f);
}

visage::Bounds SmoothHarmonicsView::barsArea() const {
	return { 2.0f, 2.0f, std::max(0.0f, static_cast<float>(width()) - 4.0f),
		     std::max(0.0f, static_cast<float>(height()) - 4.0f - kCaptionHeight) };
}

visage::Bounds SmoothHarmonicsView::barBounds(int harmonic) const {
	const visage::Bounds a = barsArea();
	const float slot = a.width() / static_cast<float>(sm::kHarmonicCount);
	const float w = std::max(2.0f, slot * 0.62f);
	const float f = fractionOfLevel(model_.harmonics[static_cast<size_t>(harmonic)]);
	const float h = a.height() * f;
	return { a.x() + slot * static_cast<float>(harmonic) + (slot - w) * 0.5f, a.bottom() - h, w, h };
}

void SmoothHarmonicsView::draw(visage::Canvas& canvas) {
	const visage::Bounds a = barsArea();
	if (a.width() <= 4.0f || a.height() <= 4.0f) {
		return;
	}
	// THE 0 dBFS LINE: a bar above it is louder than the full voice that made it.
	const float zero = a.bottom() - a.height() * fractionOfLevel(1.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.segment(a.x(), zero, a.right(), zero, 1.0f, true);
	canvas.segment(a.x(), a.bottom(), a.right(), a.bottom(), 1.0f, true);
	const float slot = a.width() / static_cast<float>(sm::kHarmonicCount);
	for (int k = 0; k < sm::kHarmonicCount; ++k) {
		const visage::Bounds bar = barBounds(k);
		// THE FUNDAMENTAL IN INK, THE HARMONICS IN THE ACCENT — what the shaper added, told from what it kept.
		canvas.setColor(k == 0 ? withAlpha(kInk, 0.7f) : withAlpha(kAccent, 0.9f));
		if (bar.height() > 0.5f) {
			canvas.rectangle(bar.x(), bar.y(), bar.width(), bar.height());
		}
		char label[4];
		std::snprintf(label, sizeof label, "%d", k + 1);
		canvas.setColor(kDisabled);
		canvas.text(label, font(kCaptionSize), visage::Font::kCenter, a.x() + slot * static_cast<float>(k), a.bottom() + 1.0f,
		            slot, kCaptionHeight);
	}
}

const char* SmoothHarmonicsView::tooltipAt(visage::Point local) const {
	// THE BARS ARE WHAT HARMONIC MOVES, so they speak Harmonic's declaration (the sweep's rule: a tip is a
	// control's declared text).
	const visage::Bounds a = barsArea();
	if (local.x < a.x() || local.x > a.right() || local.y < a.y() || local.y > a.bottom() + kCaptionHeight) {
		return nullptr;
	}
	return sm::kParamDescs[static_cast<size_t>(sm::Param::Harmonic)].tooltip;
}

} // namespace feathers::ui

namespace feathers::ui {

std::unique_ptr<ZoneCoordCell> makeSmoothCoordCell(device::Modulatable& coordinate, fx::smooth::Param param,
                                                   std::string label, float faceDiameter) {
	const device::ParamDesc& desc = fx::smooth::kParamDescs[static_cast<size_t>(param)];
	std::vector<ZoneSelect::ZoneName> unnamed(desc.zones); // regions, none named: the sigil, never a dropdown
	auto cell = std::make_unique<ZoneCoordCell>(coordinate, std::move(label), std::move(unnamed),
	                                            ZoneSelect::kCoordinatePerLap);
	cell->setFaceDiameter(faceDiameter);
	return cell;
}

} // namespace feathers::ui
