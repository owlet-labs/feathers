/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/mod_readout.h"
#include "plugin/ui/mod_readout_layer.h"

#include "device/route_resolve.h"
#include "plugin/ui/celestial.h" // reducedMotionRequested
#include "plugin/ui/control_face.h"
#include "plugin/ui/theme.h"

#include <visage_graphics/canvas.h>
#include <visage_utils/string_utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace feathers::ui {

//------------------------------------------------------------------------------------------------//
// THE ARITHMETIC — the engine's two functions, never a restatement.                              //
//------------------------------------------------------------------------------------------------//

double asModulated(const device::ParamDesc& p, double base, const backplane::RouteProbe& probe, float amount,
                   device::Transfer transfer, float source) noexcept {
	// THE ENGINE'S ORDER: the inputs before the watched one, the watched one, the rest and the host's modulation.
	double offset = probe.before;
	offset += device::routeTerm(amount, transfer, source, probe.span);
	offset += probe.after;
	return device::resolveRouted(p, base, offset);
}

ModReadout readoutFor(const device::ParamDesc& p, double base, const backplane::RouteProbe& probe, float amount,
                      device::Transfer transfer, bool bipolar, const std::function<std::string(double)>& format) {
	ModReadout r;
	r.amount = amount;
	r.value = asModulated(p, base, probe, amount, transfer, static_cast<float>(probe.source));
	// THE SWEEP: what the source can say is −1..1 or 0..1, and every curve's extremes over that lie at an end or at
	// zero (`ModDisplay::reachLo` reads the same three), while the landing (`withRouteOffset`) is monotonic in the
	// offset — so these three readings bound every value the route can reach.
	// A φ CURVE PEAKS INSIDE THE RANGE (backlog PY): its peak reading and its mirror join the three.
	const float peak = device::phiPeakReading(transfer);
	const float ends[5] = { bipolar ? -1.0f : 0.0f, 0.0f, 1.0f, peak, bipolar ? -peak : peak };
	r.lo = r.value;
	r.hi = r.value;
	for (const float s : ends) {
		const double v = asModulated(p, base, probe, amount, transfer, s);
		r.lo = std::min(r.lo, v);
		r.hi = std::max(r.hi, v);
	}
	r.text = format(r.value);
	r.loText = format(r.lo);
	r.hiText = format(r.hi);
	return r;
}

ModReadout coreReadoutFor(double base, const backplane::RouteProbe& probe, float amount, device::Transfer transfer,
                          bool bipolar, CoreTerm term, const CoreLand& land,
                          const std::function<std::string(double)>& format) {
	// `ParamSet::resolved`'s shape: (base + the host's modulation) + the routes in input order, landed.
	const auto at = [&](double reading) {
		double offset = probe.before;
		offset += term(amount, transfer, reading, probe.macro, probe.span);
		offset += probe.after;
		return land(base + probe.baseOffset + offset);
	};
	ModReadout r;
	r.amount = amount;
	r.value = at(probe.source);
	// THE SWEEP, over what the core resolve READS: any source over its polarity — a bipolar MACRO too, which the
	// core resolve reads as 2v - 1 (`ParamSet::macroReading`, bipolar-macro-core 2026-11-07; it read the raw 0..1
	// position before, so this sweep left macros at 0..1 to match).
	// A φ CURVE PEAKS INSIDE THE RANGE (backlog PY): its peak reading, and its mirror for a bipolar source, join the
	// ends (a fixed curve's peak reading is 1, already there).
	const double peak = static_cast<double>(device::phiPeakReading(transfer));
	const double lo = bipolar ? -1.0 : 0.0;
	const double ends[5] = { lo, 0.0, 1.0, peak, lo < 0.0 ? -peak : peak };
	r.lo = r.value;
	r.hi = r.value;
	for (const double s : ends) {
		const double v = at(s);
		r.lo = std::min(r.lo, v);
		r.hi = std::max(r.hi, v);
	}
	r.text = format(r.value);
	r.loText = format(r.lo);
	r.hiText = format(r.hi);
	return r;
}

//------------------------------------------------------------------------------------------------//
// WHICH ROUTE IS LIVE                                                                            //
//------------------------------------------------------------------------------------------------//

namespace {

double steadyMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct LiveState {
	std::optional<LiveRoute> route;
	double touchedAt = 0.0;
	std::function<double()> clock;
};

LiveState& live() {
	static LiveState state;
	return state;
}

bool same(const LiveRoute& r, const void* host, device::Source s, device::Destination d) {
	return r.host == host && r.source == s && r.destination == d;
}

} // namespace

double LiveAmount::now() {
	return live().clock ? live().clock() : steadyMs();
}

void LiveAmount::setClockForTest(std::function<double()> clock) {
	live().clock = std::move(clock);
}

void LiveAmount::begin(const void* host, device::Source source, device::Destination destination) {
	live().route = LiveRoute { host, source, destination, true };
	live().touchedAt = now();
}

void LiveAmount::touch(const void* host, device::Source source, device::Destination destination) {
	LiveState& s = live();
	// A WRITE INSIDE THE OPEN GESTURE keeps it held; any other write is a fresh route that lingers.
	const bool held = s.route.has_value() && s.route->held && same(*s.route, host, source, destination);
	s.route = LiveRoute { host, source, destination, held };
	s.touchedAt = now();
}

void LiveAmount::end(const void* host, device::Source source, device::Destination destination) {
	LiveState& s = live();
	if (s.route.has_value() && same(*s.route, host, source, destination)) {
		s.route->held = false;
		s.touchedAt = now();
	}
}

std::optional<LiveRoute> LiveAmount::current() {
	const LiveState& s = live();
	if (!s.route.has_value()) {
		return std::nullopt;
	}
	if (s.route->held || now() - s.touchedAt < kLingerMs) {
		return s.route;
	}
	return std::nullopt;
}

void LiveAmount::reset() {
	live().route.reset();
	live().touchedAt = 0.0;
}

//------------------------------------------------------------------------------------------------//
// THE BOX                                                                                        //
//------------------------------------------------------------------------------------------------//

namespace {
/// THE ONE LAYER SHOWING A BOX — layers nest (a Rack editor inside Feathers' own, a card inside an editor) and each
/// would find the same face; the first to show keeps it until it hides. UI thread.
ModReadoutLayer* gShowing = nullptr;
} // namespace

ModReadoutLayer::~ModReadoutLayer() {
	if (gShowing == this) {
		gShowing = nullptr;
	}
}

ModReadoutLayer::ModReadoutLayer() {
	setOnTop(true);
	setIgnoresMouseEvents(true, false);
	setVisible(false);
}

void ModReadoutLayer::watch(visage::Frame& host) {
	host.onResize() += [this, &host] { cover(host); };
	onDpiChange() += [this, &host] {
		cover(host);
		size_ = {};
	};
}

void ModReadoutLayer::cover(const visage::Frame& host) {
	setBounds(0.0f, 0.0f, host.width(), host.height());
}

namespace {

/// DRAWN, as far as this host can tell: the frame and every ancestor up to (not including) the host visible.
bool drawnUnder(const visage::Frame* f, const visage::Frame* host) {
	for (; f != nullptr && f != host; f = f->parent()) {
		if (!f->isVisible()) {
			return false;
		}
	}
	return f == host;
}

/// THE FIRST DRAWN FACE OF THE ROUTE'S DESTINATION under `frame`, depth first, skipping `skip` (this layer).
RouteReadoutSubject* walk(visage::Frame* frame, const visage::Frame* host, const visage::Frame* skip,
                          const LiveRoute& live, visage::Frame** at, std::vector<ControlFace::Offered>& scratch) {
	if (frame == skip || !frame->isVisible()) {
		return nullptr;
	}
	if (const auto* face = dynamic_cast<const ControlFace*>(frame); face != nullptr && frame != host) {
		scratch.clear();
		face->offeredControls(scratch);
		for (const ControlFace::Offered& o : scratch) {
			if (o.road != ControlFace::Road::Face || o.control == nullptr) {
				continue;
			}
			// A CONTROL FOUND IN THE LIVE TREE, so the pointer is good for this pass.
			auto* subject = dynamic_cast<RouteReadoutSubject*>(const_cast<device::Modulatable*>(o.control));
			if (subject != nullptr && subject->readoutIdentity() == live.host
			    && subject->readoutDestination() == live.destination && drawnUnder(frame, host)) {
				*at = frame;
				return subject;
			}
		}
	}
	for (visage::Frame* child : frame->children()) {
		if (RouteReadoutSubject* found = walk(child, host, skip, live, at, scratch); found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

} // namespace

RouteReadoutSubject* ModReadoutLayer::findFace(const LiveRoute& route, visage::Bounds& where) const {
	// THE HOST IS WHATEVER THIS LAYER IS ATTACHED TO, asked each time — never a remembered pointer (a window can be
	// rebuilt under it).
	visage::Frame* host = parent();
	if (host == nullptr) {
		return nullptr;
	}
	std::vector<ControlFace::Offered> scratch;
	visage::Frame* at = nullptr;
	RouteReadoutSubject* subject = walk(host, host, this, route, &at, scratch);
	if (subject != nullptr) {
		where = host->relativeBounds(at);
	}
	return subject;
}

bool ModReadoutLayer::hide() {
	if (!shown_) {
		return false;
	}
	shown_ = false;
	if (gShowing == this) {
		gShowing = nullptr;
	}
	visage::Bounds unused;
	if (RouteReadoutSubject* subject = findFace(lastLive_, unused); subject != nullptr) {
		subject->readoutEnded();
	}
	valueLine_.clear();
	rangeLine_.clear();
	setVisible(false);
	redraw();
	return true;
}

bool ModReadoutLayer::advance() {
	const std::optional<LiveRoute> route = LiveAmount::current();
	if (!route.has_value()) {
		return hide();
	}
	visage::Bounds where;
	RouteReadoutSubject* subject = findFace(*route, where);
	if (subject == nullptr) {
		return hide();
	}
	if (gShowing != nullptr && gShowing != this) {
		return hide(); // another layer already shows this route's box
	}
	const std::optional<ModReadout> r = subject->routeReadout(route->source);
	if (!r.has_value()) {
		// NO ROUTE (dragged back to nothing) OR A HOST THAT CANNOT SAY YET: nothing rather than a guess.
		return hide();
	}
	const bool newRoute = !shown_ || route->host != lastLive_.host || route->source != lastLive_.source
	                      || route->destination != lastLive_.destination;
	lastLive_ = *route;
	const double t = LiveAmount::now();
	const bool hand = r->amount != lastAmount_;
	const double every = reducedMotionRequested() ? kReducedSourceRefreshMs : kSourceRefreshMs;
	const std::string value = r->text;
	const std::string range = r->sweeps() ? r->loText + " … " + r->hiText : std::string();
	const bool changed = value != valueLine_ || range != rangeLine_;
	bool told = false;
	if (newRoute || (changed && (hand || t - adoptedAt_ >= every))) {
		readout_ = *r;
		lastAmount_ = r->amount;
		adoptedAt_ = t;
		if (changed || size_.x <= 0.0f) {
			valueLine_ = value;
			rangeLine_ = range;
			measure();
		}
		told = true;
	}
	if (where != anchor_ || newRoute) {
		anchor_ = where;
		told = true;
	}
	if (told) {
		place();
		shown_ = true;
		gShowing = this;
		setVisible(true);
		redraw();
	}
	return told;
}

void ModReadoutLayer::measure() {
	const float scale = dpiScale() > 0.0f ? dpiScale() : 1.0f;
	const visage::Font scaled = font(kKnobReadoutSize).withDpiScale(scale);
	float widest = scaled.stringWidth(visage::String::convertToUtf32(valueLine_));
	if (!rangeLine_.empty()) {
		widest = std::max(widest, scaled.stringWidth(visage::String::convertToUtf32(rangeLine_)));
	}
	const float lines = rangeLine_.empty() ? 1.0f : 2.0f;
	size_ = { std::ceil(widest) + 2.0f * kPadX, scaled.lineHeight() * lines + 2.0f * kPadY };
}

void ModReadoutLayer::place() {
	const float hostW = width();
	const float hostH = height();
	const float w = std::min(size_.x, std::max(0.0f, hostW - 2.0f * kMargin));
	const float h = size_.y;
	// ABOVE THE FACE, centred on it; BELOW when there is no room above; clamped inside the host. Never over the face
	// unless the host has no room on either side.
	const float cx = anchor_.x() + anchor_.width() * 0.5f;
	const float x = std::clamp(cx - w * 0.5f, kMargin, std::max(kMargin, hostW - w - kMargin));
	float y = anchor_.y() - h - kGap;
	if (y < kMargin) {
		y = anchor_.bottom() + kGap;
	}
	y = std::clamp(y, kMargin, std::max(kMargin, hostH - h - kMargin));
	box_ = { x, y, w, h };
}

void ModReadoutLayer::draw(visage::Canvas& canvas) {
	if (!shown_ || valueLine_.empty()) {
		return;
	}
	// THE TOOLTIP'S BOX, BORDERED IN THE MODULATION COLOUR — it is about a route, not about the control.
	canvas.setColor(withAlpha(kSurface, 0.96f));
	canvas.roundedRectangle(box_.x(), box_.y(), box_.width(), box_.height(), 3.0f);
	canvas.setColor(withAlpha(kModulation, 0.9f));
	canvas.roundedRectangleBorder(box_.x(), box_.y(), box_.width(), box_.height(), 3.0f, 1.0f);
	const visage::Font& face = font(kKnobReadoutSize);
	const float lines = rangeLine_.empty() ? 1.0f : 2.0f;
	const float lineH = (box_.height() - 2.0f * kPadY) / lines;
	canvas.setColor(kInk);
	canvas.text(valueLine_, face, visage::Font::kCenter, box_.x(), box_.y() + kPadY, box_.width(), lineH);
	if (!rangeLine_.empty()) {
		canvas.setColor(withAlpha(kInk, 0.6f));
		canvas.text(rangeLine_, face, visage::Font::kCenter, box_.x(), box_.y() + kPadY + lineH, box_.width(), lineH);
	}
}

} // namespace feathers::ui
