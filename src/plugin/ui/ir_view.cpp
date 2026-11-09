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

#include "plugin/ui/ir_view.h"

#include "fx/conv/test_ir.h"
#include "fx/featherverb/featherverb.h"
#include "mod/modulator.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace mseg = mod::mseg;
namespace fv = fx::featherverb;

namespace {

constexpr float kPadX = 6.0f;
constexpr float kPadY = 4.0f;
/// The markers' grips: a tab at the top of each line, and how near the hand must come to catch one.
constexpr float kGripHeight = 9.0f;
constexpr float kMarkerGrabPixels = 7.0f;
/// The envelope's breakpoints: the same reach the MSEG card gives its handles.
constexpr float kGrabPixels = 13.0f;
/// How far a hand travels for a full bend, as the MSEG card measures it: the box's own height.
constexpr float kCurveTravelForFullBend = 1.0f;

} // namespace

IrView::IrView(Surface surface, Edited edited) : surface_(std::move(surface)), edited_(std::move(edited)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		fv::Featherverb* verb = surface_ != nullptr ? surface_() : nullptr;
		// NO BOX TEST: the frame only hears its own presses, and a marker at the box's right edge sits ON the
		// edge, which `contains` excludes — a hand aiming at the end marker would have missed it.
		if (verb == nullptr) {
			return;
		}
		before_ = verb->irShape();
		preview_ = before_;
		const visage::Bounds span = spanBounds();
		const auto [ux, uy] = unitOf(e.position);
		const mseg::Settings env = envelopeOf(before_);
		const mseg::Layout laid = mseg::layoutOf(env);
		const float grabX = kGrabPixels / std::max(span.width(), 1.0f);
		const float grabY = kGrabPixels / std::max(span.height(), 1.0f);

		// OPT BENDS, as on the MSEG card: a breakpoint bends both its sides, a segment bends itself.
		if (e.isAltDown()) {
			curve_ = mseg::curveTargetAt(laid, ux, uy, grabX, grabY, false);
			if (curve_.kind != mseg::CurveTarget::Kind::None) {
				drag_ = Drag::Curve;
				curveAnchorY_ = e.position.y;
				previewing_ = true;
			}
			return;
		}
		// DOUBLE-CLICK ADDS A BREAKPOINT, OR REMOVES THE ONE IT LANDED ON — committed at once, it is not a drag.
		if (e.repeatClickCount() >= 2 && span.contains(e.position)) {
			const int handle = mseg::handleAt(laid, ux, uy, grabX, grabY, false);
			const mseg::Settings next = handle >= 0 ? mseg::withWaypointRemoved(env, static_cast<size_t>(handle))
			                                        : mseg::withWaypointAdded(env, ux);
			preview_ = withEnvelope(before_, next);
			previewing_ = true;
			commit();
			return;
		}
		// A BREAKPOINT FIRST, THEN A MARKER: the markers are full-height lines, so a hand aiming at one away
		// from the envelope's dots still catches it (the MSEG card's order, for its reason).
		const int handle = mseg::handleAt(laid, ux, uy, grabX, grabY, false);
		if (handle >= 0 && span.contains(e.position)) {
			drag_ = Drag::Handle;
			handle_ = handle;
			previewing_ = true;
			return;
		}
		const float bx = xOfFraction(before_.begin);
		const float ex = xOfFraction(before_.end);
		if (std::fabs(e.position.x - bx) <= kMarkerGrabPixels && std::fabs(e.position.x - bx) <= std::fabs(e.position.x - ex)) {
			drag_ = Drag::Begin;
			previewing_ = true;
		}
		else if (std::fabs(e.position.x - ex) <= kMarkerGrabPixels) {
			drag_ = Drag::End;
			previewing_ = true;
		}
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (drag_ == Drag::None) {
			return;
		}
		// THE MARKERS KEEP A SLIVER BETWEEN THEM: a span of nothing is a silent IR, which the device refuses.
		constexpr float kMinSpan = 0.01f;
		const float f = std::clamp(fractionOfX(e.position.x), 0.0f, 1.0f);
		if (drag_ == Drag::Begin) {
			preview_.begin = std::min(f, preview_.end - kMinSpan);
			preview_.begin = std::max(preview_.begin, 0.0f);
		}
		else if (drag_ == Drag::End) {
			preview_.end = std::max(f, preview_.begin + kMinSpan);
			preview_.end = std::min(preview_.end, 1.0f);
		}
		else if (drag_ == Drag::Handle) {
			const auto [ux, uy] = unitOf(e.position);
			preview_ = withEnvelope(preview_, mseg::withHandleMoved(envelopeOf(preview_), static_cast<size_t>(handle_),
			                                                       std::clamp(ux, 0.0f, 1.0f), std::clamp(uy, 0.0f, 1.0f)));
		}
		else if (drag_ == Drag::Curve) {
			// FROM THE ANCHOR, NOT FROM THE LAST EVENT — the MSEG card's rule. Up is fuller.
			const float span = std::max(spanBounds().height() * kCurveTravelForFullBend, 1.0f);
			const float delta = (curveAnchorY_ - e.position.y) / span * mod::kShapeMax;
			preview_ = withEnvelope(preview_, mseg::withCurvature(envelopeOf(before_), curve_, delta));
		}
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (drag_ == Drag::None) {
			return;
		}
		drag_ = Drag::None;
		handle_ = -1;
		curve_ = {};
		commit();
	};
}

void IrView::commit() {
	fv::Featherverb* verb = surface_ != nullptr ? surface_() : nullptr;
	if (verb != nullptr && previewing_ && !(preview_ == before_)) {
		// A shape the device refuses (a silent span) leaves it as it was, and the view follows the device.
		if (verb->setIrShape(preview_) && edited_) {
			edited_();
		}
	}
	previewing_ = false;
	redraw();
}

void IrView::dropFiles(const std::vector<std::string>& paths) {
	fv::Featherverb* verb = surface_ != nullptr ? surface_() : nullptr;
	if (verb == nullptr || paths.empty()) {
		return;
	}
	const bool loaded = verb->loadIr(paths.front());
	error_ = loaded ? std::string() : verb->irError();
	if (loaded && edited_) {
		edited_();
	}
	redraw();
}

bool IrView::refresh() {
	fv::Featherverb* verb = surface_ != nullptr ? surface_() : nullptr;
	if (verb == nullptr || previewing_) {
		return false;
	}
	if (verb->sourceSerial() != cachedSerial_ || !(verb->irShape() == shownShape_)) {
		redraw();
		return true;
	}
	return false;
}

visage::Bounds IrView::waveBounds() const {
	return { kPadX, kPadY + kGripHeight, std::max(0.0f, width() - 2.0f * kPadX),
		     std::max(0.0f, height() - 2.0f * kPadY - kGripHeight) };
}

float IrView::xOfFraction(float f) const {
	const visage::Bounds w = waveBounds();
	return w.x() + f * w.width();
}

float IrView::fractionOfX(float x) const {
	const visage::Bounds w = waveBounds();
	return w.width() > 0.0f ? (x - w.x()) / w.width() : 0.0f;
}

visage::Bounds IrView::spanBounds() const {
	const visage::Bounds w = waveBounds();
	const fx::conv::IrShape& s = previewing_ ? preview_ : shownShape_;
	const float x0 = xOfFraction(s.begin);
	const float x1 = xOfFraction(s.end);
	return { x0, w.y(), std::max(1.0f, x1 - x0), w.height() };
}

std::pair<float, float> IrView::unitOf(visage::Point p) const {
	const visage::Bounds b = spanBounds();
	return { (p.x - b.x()) / std::max(b.width(), 1.0f), 1.0f - (p.y - b.y()) / std::max(b.height(), 1.0f) };
}

visage::Point IrView::pointOf(float ux, float uy) const {
	const visage::Bounds b = spanBounds();
	return { b.x() + ux * b.width(), b.y() + (1.0f - uy) * b.height() };
}

mseg::Settings IrView::envelopeOf(const fx::conv::IrShape& shape) const {
	return fv::Featherverb::envelopeSettingsOf(shape);
}

fx::conv::IrShape IrView::withEnvelope(fx::conv::IrShape shape, const mseg::Settings& env) const {
	return fv::Featherverb::shapeOfEnvelope(shape, env);
}

void IrView::cachePeaks(const fv::Featherverb& verb) {
	const fx::conv::LoadedIr& whole = verb.irWhole();
	const visage::Bounds w = waveBounds();
	const auto columns = static_cast<size_t>(std::max(1.0f, std::floor(w.width())));
	peaks_.assign(columns, { 0.0f, 0.0f });
	const size_t n = std::min(whole.left.size(), whole.right.size());
	float peak = 0.0f;
	for (size_t c = 0; c < columns && n > 0; ++c) {
		const size_t from = c * n / columns;
		const size_t to = std::max(from + 1, (c + 1) * n / columns);
		float lo = 0.0f;
		float hi = 0.0f;
		for (size_t i = from; i < to && i < n; ++i) {
			const float v = 0.5f * (whole.left[i] + whole.right[i]);
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
		peaks_[c] = { lo, hi };
		peak = std::max({ peak, -lo, hi });
	}
	// SCALED TO ITS OWN PEAK: an IR's level is normalised anyway, so the picture is its shape.
	if (peak > 0.0f) {
		for (auto& p : peaks_) {
			p.first /= peak;
			p.second /= peak;
		}
	}
	cachedSerial_ = verb.sourceSerial();
	cachedWidth_ = w.width();
}

void IrView::draw(visage::Canvas& canvas) {
	const visage::Bounds w = waveBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(w.x(), w.y(), w.width(), w.height(), 4.0f);
	fv::Featherverb* verb = surface_ != nullptr ? surface_() : nullptr;
	if (verb == nullptr) {
		return;
	}
	if (verb->sourceSerial() != cachedSerial_ || cachedWidth_ != w.width()) {
		cachePeaks(*verb);
	}
	if (!previewing_) {
		shownShape_ = verb->irShape();
	}
	const fx::conv::IrShape& s = previewing_ ? preview_ : shownShape_;

	// THE WAVEFORM: brighter inside the span the markers keep, dim outside it.
	const float mid = w.y() + 0.5f * w.height();
	const float half = 0.5f * w.height() - 2.0f;
	const float x0 = xOfFraction(s.begin);
	const float x1 = xOfFraction(s.end);
	for (size_t c = 0; c < peaks_.size(); ++c) {
		const float x = w.x() + static_cast<float>(c) + 0.5f;
		const bool inside = x >= x0 && x <= x1;
		canvas.setColor(withAlpha(kValue, inside ? 0.55f : 0.16f));
		const float top = mid - peaks_[c].second * half;
		const float bottom = mid - peaks_[c].first * half;
		canvas.segment(x, top, x, std::max(bottom, top + 1.0f), 1.0f, false);
	}

	// THE MARKERS: a full-height line and a grip above the box.
	canvas.setColor(withAlpha(kVoid, 0.45f));
	canvas.rectangle(w.x(), w.y(), std::max(0.0f, x0 - w.x()), w.height());
	canvas.rectangle(x1, w.y(), std::max(0.0f, w.right() - x1), w.height());
	canvas.setColor(kAccent);
	for (const float x : { x0, x1 }) {
		canvas.segment(x, w.y(), x, w.bottom(), 1.2f, true);
		canvas.rectangle(x - 3.5f, kPadY, 7.0f, kGripHeight);
	}

	// THE ENVELOPE OVER THE SPAN — the MSEG's own polyline and handles, in the span's box.
	const mseg::Settings env = envelopeOf(s);
	const auto line = mseg::polyline(env, 16);
	canvas.setColor(kInk);
	for (size_t i = 1; i < line.size(); ++i) {
		const visage::Point a = pointOf(line[i - 1].first, line[i - 1].second);
		const visage::Point b = pointOf(line[i].first, line[i].second);
		canvas.segment(a.x, a.y, b.x, b.y, 1.4f, true);
	}
	const mseg::Layout laid = mseg::layoutOf(env);
	for (const mseg::Handle& h : laid.handle) {
		const visage::Point at = pointOf(h.x, h.level);
		canvas.setColor(kInk);
		canvas.circle(at.x - 3.0f, at.y - 3.0f, 6.0f);
	}

	// THE FILE'S STATUS, in the corner — the strip's browser shows the name, this says what is wrong with it.
	{
		std::string status;
		if (!error_.empty()) {
			status = "could not load: " + error_;
		}
		else if (!verb->irPath().empty() && !verb->irError().empty()) {
			status = "missing (" + verb->irError() + ")";
		}
		else if (verb->irTruncated()) {
			status = "cut at " + std::to_string(static_cast<int>(fx::featherverb::kConvMaxIrSeconds)) + " s";
		}
		if (!status.empty()) {
			canvas.setColor(withAlpha(kMuted, 0.95f));
			canvas.text(status, font(kKnobReadoutSize), visage::Font::kLeft, w.x() + 4.0f, w.y() + 2.0f,
			            w.width() - 8.0f, 12.0f);
		}
	}

	// NOTHING LOADED YET: say how to load something.
	if (verb->irPath().empty()) {
		canvas.setColor(withAlpha(kMuted, 0.9f));
		canvas.text(std::string(fx::conv::generatedRoomName(verb->generatedRoom())) + " — ◀ ▶ to browse, or drop a WAV, AIFF or FLAC here",
		            font(kKnobReadoutSize), visage::Font::kCenter,
		            w.x(), w.bottom() - 14.0f, w.width(), 12.0f);
	}
}

} // namespace feathers::ui
