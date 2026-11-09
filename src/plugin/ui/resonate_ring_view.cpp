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

#include "plugin/ui/resonate_ring_view.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace feathers::ui {

namespace rs = fx::resonate;

namespace {

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
constexpr size_t kChain = rs::kChainNodes;

/// WHETHER ANY STRING SOUNDS — a played note's head or a drone string: what shows the travel marker.
bool anySounding(const rs::Reading& r) {
	const auto on = [](float g) { return g > 0.0f; };
	return std::any_of(r.headGains.begin(), r.headGains.end(), on)
	       || std::any_of(r.droneGains.begin(), r.droneGains.end(), on);
}
/// THE MEMBRANE'S GRID, and the scan circle's radius in its coordinates (the rim at +-1) —
/// `weave_models.cpp`'s `kGrid` and `kCircleRadius`, restated because the view must not link the DSP.
constexpr size_t kGrid = 8;
constexpr float kCircleRadius = 0.6f;
/// HOW WIDE THE MORPH BOX IS IN THE TOP BAND, and the Character box in the bottom one — the face seats them.
constexpr float kMorphBoxWidth = 82.0f;
constexpr float kCharacterBoxWidth = 96.0f;

/// THE OUT-AND-BACK READ Bar and Tube scan: 0 -> 1 over the first half of the path, 1 -> 0 over the second.
[[nodiscard]] float outAndBack(float pos) {
	const float p = pos - std::floor(pos);
	return p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
}

/// LINEAR READ OF THE FIRST `kChain` NODES at fractional index `u`, cyclic for a loop.
[[nodiscard]] float chainAt(const std::array<float, rs::kPictureNodes>& n, float u, bool cyclic) {
	if (cyclic) {
		u -= std::floor(u / static_cast<float>(kChain)) * static_cast<float>(kChain);
		const size_t i = std::min(static_cast<size_t>(u), kChain - 1);
		const float f = u - static_cast<float>(i);
		return n[i] + (n[(i + 1) % kChain] - n[i]) * f;
	}
	u = std::clamp(u, 0.0f, static_cast<float>(kChain - 1));
	const size_t i = std::min(static_cast<size_t>(u), kChain - 2);
	const float f = u - static_cast<float>(i);
	return n[i] + (n[i + 1] - n[i]) * f;
}

/// A MEMBRANE HEIGHT, as a colour: the accent above rest, the value colour below, alpha by size.
[[nodiscard]] uint32_t heightColour(float v) {
	const float a = std::clamp(std::fabs(v), 0.0f, 1.0f);
	return withAlpha(v >= 0.0f ? kAccent : kValue, 0.08f + 0.62f * a);
}

[[nodiscard]] float normalised(const device::Modulatable& c, double v) {
	const double span = c.desc().max - c.desc().min;
	return span > 0.0 ? std::clamp(static_cast<float>((v - c.desc().min) / span), 0.0f, 1.0f) : 0.0f;
}

} // namespace

ResonateRingView::ResonateRingView(const backplane::DeviceHost& host, DeviceInstanceId id,
                                   device::Modulatable& character, device::Modulatable& morph)
    : host_(host), device_(id), character_(character), morph_(morph) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// DOUBLE-CLICK RESETS THE PAIR — a knob's gesture, for the two knobs this surface is.
		if (e.repeatClickCount() >= 2) {
			for (device::Modulatable* c : { &character_, &morph_ }) {
				c->beginGesture();
				c->setValue(c->desc().def);
				c->endGesture();
			}
			(void)refresh();
			return;
		}
		dragging_ = true;
		anchor_ = e.position;
		anchorCharacter_ = character_.value();
		anchorMorph_ = morph_.value();
		character_.beginGesture();
		morph_.beginGesture();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!dragging_) {
			return;
		}
		const visage::Bounds box = plotBounds();
		// BY THE HAND, NOT TO IT: one plot width of travel is the whole of Character, one plot height the
		// whole of Morph (up is more). Shift is the knobs' fine factor.
		const double fine = e.isShiftDown() ? 0.2 : 1.0;
		const double dx = (e.position.x - anchor_.x) / std::max(1.0f, box.width()) * fine;
		const double dy = (anchor_.y - e.position.y) / std::max(1.0f, box.height()) * fine;
		const auto clamped = [](const device::Modulatable& c, double v) {
			return std::clamp(v, c.desc().min, c.desc().max);
		};
		character_.setValue(clamped(character_, anchorCharacter_ + dx));
		morph_.setValue(clamped(morph_, anchorMorph_ + dy));
		(void)refresh();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!dragging_) {
			return;
		}
		dragging_ = false;
		character_.endGesture();
		morph_.endGesture();
	};
	// A SCALE CHANGE MOVES NOTHING LOGICAL, so the bounds are re-set to convert the native rectangle at the
	// new scale (CLAUDE.md's setBounds/DPI rule; `setBounds` dedups on both rectangles).
	onDpiChange() += [this] { setBounds(bounds()); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

const char* ResonateRingView::tooltipAt(visage::Point local) const {
	// THE AXES ARE NOT ANSWERED HERE: Character's and Morph's boxes sit over the bands as later siblings, so a
	// hover there reaches the box and the box speaks its own declaration. What this view answers is a note
	// chip (the note that head sounds) and, everywhere else, the drag that moves both.
	for (const Chip& c : chips()) {
		if (c.box.contains(local)) {
			chipTip_ = std::string(c.drone ? kDroneChipTooltipPrefix : kChipTooltipPrefix)
			           + formatValue(rs::kParamDescs[static_cast<size_t>(rs::Param::Root)], c.note);
			return chipTip_.c_str();
		}
	}
	return kPlotTooltip;
}

visage::Bounds ResonateRingView::plotBounds() const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	// A PUCK'S RADIUS CLEAR OF EACH BAND, so the puck at Morph 0 or 1 never sits on a box.
	const float pad = kPuckDiameter * 0.5f + 1.0f;
	// AND CLEAR OF ANY CHIP ROWS PAST THE FIRST (chips()), which the plot gives up its bottom edge for.
	const float chipRowsAbove = kBand * static_cast<float>(chipRows() - 1);
	return { kInset, kInset + kBand + pad, std::max(0.0f, w - 2.0f * kInset),
		     std::max(0.0f, h - 2.0f * (kInset + kBand + pad) - chipRowsAbove) };
}

visage::Bounds ResonateRingView::morphBand() const {
	return { kInset, kInset, std::min(kMorphBoxWidth, std::max(0.0f, static_cast<float>(width()) - 2.0f * kInset)),
		     kBand - 2.0f };
}

visage::Bounds ResonateRingView::characterBand() const {
	return { kInset, static_cast<float>(height()) - kInset - kBand + 2.0f,
		     std::min(kCharacterBoxWidth, std::max(0.0f, static_cast<float>(width()) - 2.0f * kInset)), kBand - 2.0f };
}

rs::Model ResonateRingView::model() const {
	const int m = std::clamp(static_cast<int>(std::lround(reading_.model)), 0,
	                         static_cast<int>(rs::Model::kCount) - 1);
	return static_cast<rs::Model>(m);
}

bool ResonateRingView::Drawn::operator==(const Drawn& o) const {
	const rs::Reading& a = reading;
	const rs::Reading& b = o.reading;
	// THE TRAVEL IS DRAWN ONLY WHILE A HEAD SOUNDS (the marker), so only then does it count: MEASURED on the
	// real DSP (2026-09-26), a still string with Bow up keeps its travel moving with nothing sounding, and a
	// comparison that read it repainted the view every tick at rest.
	const bool travelShown = anySounding(a) || anySounding(b);
	return published == o.published && flash == o.flash && character == o.character && morph == o.morph
	       && characterBase == o.characterBase && morphBase == o.morphBase && a.model == b.model
	       && a.nodes == b.nodes && a.rest == b.rest && (!travelShown || a.travel == b.travel)
	       && a.headGains == b.headGains && a.headNotes == b.headNotes && a.droneGains == b.droneGains
	       && a.droneNotes == b.droneNotes && a.droneCount == b.droneCount && a.parked == b.parked
	       && (flash <= 0.0f || a.pluckPos == b.pluckPos);
}

ResonateRingView::Drawn ResonateRingView::snapshot() const {
	Drawn d;
	d.reading = reading_;
	d.published = published_;
	d.flash = flashAlpha();
	const device::ModDisplay c = character_.display();
	const device::ModDisplay m = morph_.display();
	d.character = c.modulated;
	d.morph = m.modulated;
	d.characterBase = c.base;
	d.morphBase = m.base;
	return d;
}

bool ResonateRingView::refresh() {
	const size_t count = host_.readingCountAt(device_);
	std::array<float, rs::kReadingCount> flat {};
	const size_t take = std::min(count, flat.size());
	for (size_t i = 0; i < take; ++i) {
		flat[i] = host_.readingAt(device_, i);
	}
	reading_ = rs::unpackReading(flat.data(), take);
	// THE MOTION AT DISPLAY GAIN — see `kMotionGain`. A function of the reading alone, so the rest-state
	// comparison below still decides every repaint.
	for (size_t i = 0; i < rs::kPictureNodes; ++i) {
		shown_[i] = reading_.rest[i] + kMotionGain * (reading_.nodes[i] - reading_.rest[i]);
	}
	published_ = take;
	const Drawn now = snapshot();
	if (drawnOnce_ && now == drawn_) {
		return false;
	}
	drawn_ = now;
	drawnOnce_ = true;
	redraw();
	return true;
}

float ResonateRingView::scale() const {
	float peak = kScaleFloor;
	const size_t n = model() == rs::Model::Membrane ? rs::kPictureNodes : kChain;
	for (size_t i = 0; i < n; ++i) {
		// THE AMPLIFIED SHAPE IS ON THE RULER, so the gain can never carry the loop out of the plot.
		peak = std::max({ peak, std::fabs(shown_[i]), std::fabs(reading_.rest[i]) });
	}
	return 1.0f / peak;
}

float ResonateRingView::membraneSide() const {
	const visage::Bounds box = plotBounds();
	return std::max(0.0f, std::min(box.width(), box.height()));
}

visage::Point ResonateRingView::pathPointAt(const std::array<float, rs::kPictureNodes>& nodes, float pos) const {
	const visage::Bounds box = plotBounds();
	const float cx = box.x() + box.width() * 0.5f;
	const float cy = box.y() + box.height() * 0.5f;
	const float s = scale();
	const float p = pos - std::floor(pos);
	switch (model()) {
	case rs::Model::Ring:
	case rs::Model::Stiffening: {
		// A LOOP, READ FROM THE TOP CLOCKWISE; displacement pushes a node out from the base circle.
		const float half = std::min(box.width(), box.height()) * 0.5f;
		const float base = half * 0.62f;
		const float swing = half * 0.34f;
		const float d = chainAt(nodes, p * static_cast<float>(kChain), true) * s;
		const float a = -0.5f * std::numbers::pi_v<float> + kTwoPi * p;
		const float r = base + swing * d;
		return { cx + r * std::cos(a), cy + r * std::sin(a) };
	}
	case rs::Model::Bar: {
		// FREE ENDS: node 0 at the left edge and node 31 at the right, read out and back.
		const float u = outAndBack(p) * static_cast<float>(kChain - 1);
		const float d = chainAt(nodes, u, false) * s;
		return { box.x() + box.width() * (u / static_cast<float>(kChain - 1)), cy - box.height() * 0.42f * d };
	}
	case rs::Model::Tube: {
		// PINNED ENDS at the box's edges; the 32 nodes sit between them, read out and back.
		const float u = outAndBack(p) * static_cast<float>(kChain - 1);
		const float d = chainAt(nodes, u, false) * s;
		return { box.x() + box.width() * ((u + 1.0f) / static_cast<float>(kChain + 1)), cy - box.height() * 0.42f * d };
	}
	case rs::Model::Membrane:
	case rs::Model::kCount: break;
	}
	// MEMBRANE: the point on the scan circle, in the square the grid is drawn in.
	const float side = membraneSide();
	const float a = kTwoPi * p;
	return { cx + side * 0.5f * kCircleRadius * std::cos(a), cy + side * 0.5f * kCircleRadius * std::sin(a) };
}

std::vector<visage::Point> ResonateRingView::pathOf(const std::array<float, rs::kPictureNodes>& nodes) const {
	std::vector<visage::Point> out;
	const rs::Model m = model();
	if (m == rs::Model::Bar || m == rs::Model::Tube) {
		// THE CHAIN ONCE, LEFT TO RIGHT — the out leg of the path; the back leg is the same points.
		const visage::Bounds box = plotBounds();
		const float cy = box.y() + box.height() * 0.5f;
		if (m == rs::Model::Tube) {
			out.push_back({ box.x(), cy });
		}
		for (size_t i = 0; i < kChain; ++i) {
			out.push_back(pathPointAt(nodes, 0.5f * static_cast<float>(i) / static_cast<float>(kChain - 1)));
		}
		if (m == rs::Model::Tube) {
			out.push_back({ box.right(), cy });
		}
		return out;
	}
	for (size_t i = 0; i < kChain; ++i) {
		out.push_back(pathPointAt(nodes, static_cast<float>(i) / static_cast<float>(kChain)));
	}
	return out;
}

visage::Bounds ResonateRingView::membraneCell(size_t node) const {
	const visage::Bounds box = plotBounds();
	const float side = membraneSide();
	const float x0 = box.x() + (box.width() - side) * 0.5f;
	const float y0 = box.y() + (box.height() - side) * 0.5f;
	const float cell = side / static_cast<float>(kGrid);
	const size_t row = node / kGrid;
	const size_t col = node % kGrid;
	return { x0 + cell * static_cast<float>(col), y0 + cell * static_cast<float>(row), cell, cell };
}

namespace {
/// A SOUNDING HEAD, as a chip will show it.
struct Sounding {
	float note;
	float gain;
	bool drone;
};
using SoundingList = std::array<Sounding, rs::kMaxHeads + rs::kMaxDroneStrings>;
/// EVERY SOUNDING HEAD, THE PLAYED NOTES FIRST, then the drone's strings in the chord's order; returns how many.
size_t collectSounding(const rs::Reading& r, SoundingList& out) {
	size_t n = 0;
	const auto add = [&](float note, float gain, bool drone) {
		const float g = std::clamp(gain, 0.0f, 1.0f);
		if (g > 0.0f) {
			out[n++] = { note, g, drone };
		}
	};
	for (size_t i = 0; i < rs::kMaxHeads; ++i) {
		add(r.headNotes[i], r.headGains[i], false);
	}
	const size_t strings = std::min(rs::kMaxDroneStrings, static_cast<size_t>(std::max(0.0f, r.droneCount)));
	for (size_t i = 0; i < strings; ++i) {
		add(r.droneNotes[i], r.droneGains[i], true);
	}
	return n;
}
} // namespace

float ResonateRingView::chipRoom() const {
	const float right = static_cast<float>(width()) - kInset;
	return std::max(0.0f, right - (characterBand().right() + kChipGap));
}

size_t ResonateRingView::chipRows() const {
	SoundingList sounding {};
	const size_t n = collectSounding(reading_, sounding);
	const size_t fitAtMin = std::max<size_t>(1, static_cast<size_t>((chipRoom() + kChipGap) / (kChipMinWidth + kChipGap)));
	return std::max<size_t>(1, (n + fitAtMin - 1) / fitAtMin);
}

std::vector<ResonateRingView::Chip> ResonateRingView::chips() const {
	// EVERY SOUNDING HEAD GETS A CHIP (at a fixed six, an eight-string raga plus held notes lost strings),
	// the played notes first.
	SoundingList sounding {};
	const size_t n = collectSounding(reading_, sounding);
	std::vector<Chip> out;
	if (n == 0) {
		return out;
	}
	// RIGHT-ALIGNED BESIDE CHARACTER'S BOX: one row on the bottom band while they fit at kChipMinWidth, narrowing from
	// kChipWidth as they crowd; past that, more rows stacked upward, the chips shared evenly among them, in room the
	// plot gives up for them (plotBounds), so neither the puck nor a cell ever sits under one. Row 0 is the band; the
	// first chip is its rightmost.
	const visage::Bounds band = characterBand();
	const float right = static_cast<float>(width()) - kInset;
	const float room = chipRoom();
	const size_t rows = chipRows();
	const size_t perRow = (n + rows - 1) / rows;
	const float w = std::clamp((room - kChipGap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow),
	                           std::min(kChipMinWidth, room), kChipWidth);
	for (size_t i = 0; i < n; ++i) {
		const float row = static_cast<float>(i / perRow);
		const float col = static_cast<float>(i % perRow);
		const visage::Bounds box { right - w - col * (w + kChipGap), band.y() - row * kBand, w, band.height() };
		out.push_back({ sounding[i].note, 0.35f + 0.65f * sounding[i].gain, box, sounding[i].drone });
	}
	return out;
}

visage::Point ResonateRingView::puckPoint(bool base) const {
	const visage::Bounds box = plotBounds();
	const device::ModDisplay c = character_.display();
	const device::ModDisplay m = morph_.display();
	const float cx = normalised(character_, base ? c.base : c.modulated);
	const float my = normalised(morph_, base ? m.base : m.modulated);
	return { box.x() + box.width() * cx, box.bottom() - box.height() * my };
}

float ResonateRingView::flashAlpha() const {
	if (!hasReading() || !(reading_.pluckAge < kFlashTicks)) {
		return 0.0f;
	}
	return std::clamp(1.0f - std::max(0.0f, reading_.pluckAge) / kFlashTicks, 0.0f, 1.0f);
}

void ResonateRingView::drawShape(visage::Canvas& canvas, const std::vector<visage::Point>& pts, bool closed,
                                 float thickness) {
	for (size_t i = 0; i + 1 < pts.size(); ++i) {
		canvas.segment(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, thickness, true);
	}
	if (closed && pts.size() > 2) {
		canvas.segment(pts.back().x, pts.back().y, pts.front().x, pts.front().y, thickness, true);
	}
}

void ResonateRingView::drawMembrane(visage::Canvas& canvas, const visage::Bounds& box) {
	// THE 8x8 HEIGHT MAP, ROW-MAJOR, ROW 0 AT THE TOP, in a centred square: each cell tinted by its height.
	const float s = scale();
	for (size_t node = 0; node < kGrid * kGrid; ++node) {
		const visage::Bounds c = membraneCell(node);
		// THE SHOWN HEIGHT (the motion at display gain), dimmed while parked.
		const uint32_t tint = heightColour(shown_[node] * s);
		canvas.setColor(parked() ? withAlpha(tint, ((tint >> 24) / 255.0f) * kParkedAlpha) : tint);
		canvas.roundedRectangle(c.x() + 1.0f, c.y() + 1.0f, c.width() - 2.0f, c.height() - 2.0f, 2.0f);
		// THE REST POSE, AS A RING IN EACH CELL whose size is the rest height — the ghost, in the one form
		// that does not fight the tint. ONLY WHERE A CELL IS BIG ENOUGH TO HOLD ONE: at card size (~16 px
		// cells) sixty-four tiny rings read as noise (the review, 2026-09-26), so the card shows the tint alone
		// and the panel, whose cells are several times larger, shows the ghost.
		const float rest = std::clamp(std::fabs(reading_.rest[node] * s), 0.0f, 1.0f);
		if (rest > 0.02f && c.width() >= kRestRingMinCell) {
			const float d = (c.width() - 6.0f) * rest;
			canvas.setColor(withAlpha(kInk, 0.22f));
			canvas.ring(c.x() + c.width() * 0.5f - d * 0.5f, c.y() + c.height() * 0.5f - d * 0.5f, d, 1.0f);
		}
	}
	// THE SCAN CIRCLE the heads read round.
	const float r = membraneSide() * 0.5f * kCircleRadius;
	const float cx = box.x() + box.width() * 0.5f;
	const float cy = box.y() + box.height() * 0.5f;
	canvas.setColor(withAlpha(kInk, 0.55f));
	canvas.ring(cx - r, cy - r, 2.0f * r, 1.2f);
}

void ResonateRingView::draw(visage::Canvas& canvas) {
	const visage::Bounds box = plotBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()), 4.0f);
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}

	// THE PUCK'S CROSSHAIR, UNDER EVERYTHING — Character across, Morph up, at the modulated pair.
	const visage::Point puck = puckPoint();
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.fill(puck.x - 0.5f, box.y(), 1.0f, box.height());
	canvas.fill(box.x(), puck.y - 0.5f, box.width(), 1.0f);

	if (!hasReading()) {
		canvas.setColor(kDisabled);
		canvas.text("no signal", font(kKnobLabelSize), visage::Font::kCenter, box.x(), box.y(), box.width(),
		            box.height());
	}
	else {
		const rs::Model m = model();
		const bool closed = m == rs::Model::Ring || m == rs::Model::Stiffening;
		if (m == rs::Model::Membrane) {
			drawMembrane(canvas, box);
		}
		else {
			// THE REST POSE, FAINT, THEN THE LIVE SHAPE OVER IT.
			canvas.setColor(withAlpha(kInk, 0.18f));
			drawShape(canvas, restPath(), closed, 1.0f);
			// DIMMED WHILE PARKED — the frozen string reads as held, like its ghost, not as a live trace.
			canvas.setColor(withAlpha(kAccent, liveAlpha()));
			drawShape(canvas, livePath(), closed, 1.8f);
			if (m == rs::Model::Tube) {
				// THE PINS, so a Tube's held ends read as held.
				const float cy = box.y() + box.height() * 0.5f;
				canvas.setColor(kMuted);
				canvas.fill(box.x() - 1.0f, cy - 6.0f, 2.0f, 12.0f);
				canvas.fill(box.right() - 1.0f, cy - 6.0f, 2.0f, 12.0f);
			}
		}

		// THE PLUCK FLASH, where it landed, fading with its age.
		const float flash = flashAlpha();
		if (flash > 0.0f) {
			const visage::Point at = scanPoint(reading_.pluckPos);
			const float d = 10.0f + 14.0f * (1.0f - flash);
			canvas.setColor(withAlpha(kInk, 0.7f * flash));
			canvas.ring(at.x - d * 0.5f, at.y - d * 0.5f, d, 1.5f);
		}

		// THE SCAN ORIGIN: one marker at the travel phase, while any head sounds.
		const std::vector<Chip> heads = chips();
		if (anySounding(reading_)) {
			const visage::Point at = travelPoint();
			canvas.setColor(kValue);
			canvas.circle(at.x - kMarkerDiameter * 0.5f, at.y - kMarkerDiameter * 0.5f, kMarkerDiameter);
		}
		// AND THE HEADS AS NOTE CHIPS, as bright as they sound: a played note filled, a drone string outlined in
		// the muted ink — the chord under the notes, visibly the quieter of the two.
		for (const Chip& c : heads) {
			if (c.drone) {
				canvas.setColor(withAlpha(kMuted, 0.6f * c.alpha));
				canvas.roundedRectangleBorder(c.box.x(), c.box.y(), c.box.width(), c.box.height(), 3.0f, 1.0f);
				canvas.setColor(withAlpha(kMuted, c.alpha));
			}
			else {
				canvas.setColor(withAlpha(kValue, 0.22f * c.alpha));
				canvas.roundedRectangle(c.box.x(), c.box.y(), c.box.width(), c.box.height(), 3.0f);
				canvas.setColor(withAlpha(kValue, c.alpha));
			}
			const std::string name = formatValue(rs::kParamDescs[static_cast<size_t>(rs::Param::Root)], c.note);
			canvas.text(name, font(kKnobLabelSize), visage::Font::kCenter, c.box.x(), c.box.y(), c.box.width(),
			            c.box.height());
		}

		// THE MODEL'S NAME, top right — Ring and Stiffening share a picture, and this tells them apart.
		const std::string name = upper(rs::kModelNames[static_cast<size_t>(m)]);
		const float w = trackedWidth(font(kKnobLabelSize), name, kKnobLabelTracking);
		canvas.setColor(withAlpha(kInk, 0.7f));
		drawTracked(canvas, name, font(kKnobLabelSize), kKnobLabelTracking,
		            static_cast<float>(width()) - kInset - w - 2.0f, kInset + 2.0f, kBand - 4.0f);
		// AND, WHILE PARKED, THE HELD MARK beside it — the one word that says the stillness is the device's.
		if (parked()) {
			const float hw = trackedWidth(font(kKnobLabelSize), kParkedMark, kKnobLabelTracking);
			canvas.setColor(withAlpha(kValue, 0.85f));
			drawTracked(canvas, kParkedMark, font(kKnobLabelSize), kKnobLabelTracking,
			            static_cast<float>(width()) - kInset - w - hw - 10.0f, kInset + 2.0f, kBand - 4.0f);
		}
	}

	// THE PUCK, ON TOP: a ring at the modulated pair, and a faint one at the base when they differ.
	const visage::Point base = puckPoint(true);
	if (std::fabs(base.x - puck.x) > 0.5f || std::fabs(base.y - puck.y) > 0.5f) {
		canvas.setColor(withAlpha(kPointer, 0.3f));
		canvas.ring(base.x - kPuckDiameter * 0.5f, base.y - kPuckDiameter * 0.5f, kPuckDiameter, 1.0f);
	}
	canvas.setColor(withAlpha(kPointer, 0.9f));
	canvas.ring(puck.x - kPuckDiameter * 0.5f, puck.y - kPuckDiameter * 0.5f, kPuckDiameter, 1.5f);
}

} // namespace feathers::ui
