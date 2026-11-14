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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/waypoint_expanded.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace wp = fx::waypoint;
namespace flt = fx::filter;

namespace {

constexpr size_t kCurvePoints = 160;

/// THE DEVICE-WIDE ROW, in its order. One table, read by the constructor and by
/// `addressedParamsForTest`, so the two cannot disagree about what is on screen.
constexpr std::array<wp::Param, 1> kDeviceKnobs {
	wp::Param::Morph,
};

[[nodiscard]] wp::Param fieldOf(size_t waypoint, size_t field) {
	return static_cast<wp::Param>(static_cast<size_t>(wp::Param::Cutoff0) + waypoint * 4 + field);
}

} // namespace

WaypointExpanded::WaypointExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                   Notifier* notifier)
    : host_(host), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — see generic_card.cpp. The parameter stays
	// until the signature pass; discarded so a deferred cleanup is not read as a forgotten use.
	(void)registry;
	controls_.resize(static_cast<size_t>(wp::Param::kCount));
	const auto addKnob = [&](wp::Param p) {
		auto knob = std::make_unique<Knob>(control(p),
		                                   wp::kParamDescs[static_cast<size_t>(p)].name, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		knobParams_.push_back(p);
	};
	// A ROW PER WAYPOINT, A COLUMN PER FIELD. Reading DOWN a column compares one quantity across the
	// four states, which is the question the morph asks — and the reason this panel exists rather
	// than being the card at a larger size.
	for (size_t w = 0; w < wp::kWaypoints; ++w) {
		for (size_t f = 0; f < kColumns; ++f) {
			addKnob(fieldOf(w, f));
		}
	}
	for (const wp::Param p : kDeviceKnobs) {
		addKnob(p);
	}
	// THE OTHER THREE OF THE DEVICE-WIDE ROW ARE CHOICES, NOT DIALS: the filter type's list and two chips.
	type_ = std::make_unique<ValueBox>(control(wp::Param::Type), "Type");
	addChild(type_.get());
	keytrack_ = std::make_unique<SwitchChip>(control(wp::Param::Keytrack), "Keytrack");
	addChild(keytrack_.get());
	poly_ = std::make_unique<SwitchChip>(control(wp::Param::Poly), "Poly");
	addChild(poly_.get());
}

DeviceControl& WaypointExpanded::control(wp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] =
		    std::make_unique<DeviceControl>(host_, device_, wp::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

void WaypointExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, including any this panel does not hold a knob for — FilterCard's rule:
	// cheap, total, and it cannot fall behind a widget list.
	for (size_t i = 0; i < static_cast<size_t>(wp::Param::kCount); ++i) {
		control(static_cast<wp::Param>(i)).setModulation(mod, history);
	}
}

visage::Point WaypointExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kRowGap };
}

visage::Point WaypointExpanded::minimumSize() const {
	const float column = Knob::columnHeight(kKnobDiameter);
	// FIVE ROWS: four waypoints and the device-wide row. Stated as a count of the rows this panel
	// actually lays out rather than as a measured total, because the overlay CLAMPS rather than
	// scrolling — a panel asking for more than it fills leaves blank canvas, and one asking for less
	// has its bottom row silently cut off.
	const float rows = static_cast<float>(wp::kWaypoints) + 1.0f;
	const float height = kPlaneHeight + kRowGap + rows * (column + kRowGap);
	return { static_cast<float>(kColumns) * kCell, height };
}

std::string WaypointExpanded::title() const {
	// THE DEVICE'S OWN NAME, asked of the descriptor rather than spelled here — the same authority
	// the card's head reads, so the two cannot come to say different things.
	return wp::kTraits.name;
}

void WaypointExpanded::resized() { layOut(); }

void WaypointExpanded::layOut() {
	const float column = Knob::columnHeight(kKnobDiameter);
	const float cell = width() / static_cast<float>(kColumns);
	const float top = kPlaneHeight + kRowGap;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const size_t row = i / kColumns;
		const size_t col = i % kColumns;
		knobs_[i]->setBounds(cell * static_cast<float>(col),
		                     top + static_cast<float>(row) * (column + kRowGap), cell, column);
	}
	// THE DEVICE-WIDE ROW'S OTHER THREE, in columns 1..3 of the row Morph starts, centred on the face's line.
	const size_t deviceRow = (wp::kWaypoints * kColumns) / kColumns;
	const float rowTop = top + static_cast<float>(deviceRow) * (column + kRowGap);
	type_->setBounds(cell * 1.0f + 2.0f, rowTop + 6.0f, cell - 4.0f, ValueBox::kHeight);
	const float chipY = rowTop + 6.0f + 0.5f * (ValueBox::kHeight - SwitchChip::kHeight);
	keytrack_->setBounds(cell * 2.0f + 2.0f, chipY, cell - 4.0f, SwitchChip::kHeight);
	poly_->setBounds(cell * 3.0f + 2.0f, chipY, cell - 4.0f, SwitchChip::kHeight);
}

void WaypointExpanded::draw(visage::Canvas& canvas) {
	const visage::Bounds box { 0.0f, 0.0f, width(), kPlaneHeight };
	if (box.width() <= 1.0f) {
		return;
	}
	// THE SAME PICTURE THE CARD DRAWS, at this panel's size — four ghosted waypoint curves and the
	// live interpolated one over them, every one through `flt::gainProfile` from a `flt::Settings`.
	// That reuse is what `settingsAtMorph` returning the filter's own type was for.
	// THE ROW, READ THROUGH THE SEAM — `fieldsPresentAt`/`fieldAt` are the chain read without the
	// container only the parameter road has. Same numbers, by construction.
	wp::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	const wp::Settings s = wp::stateOf(row, present);

	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(box.x(), box.y(), box.width(), box.height(), 1.0f);

	const auto drawCurve = [&](const flt::Settings& fs, uint32_t colour, float thickness) {
		std::array<float, kCurvePoints> freq {};
		std::array<float, kCurvePoints> gain {};
		// AT THE RATE THE ENGINE IS RUNNING, never a hardcoded 48000 — filter_card.cpp measured up to
		// 1.67 dB between the drawn curve and the filter that plays on an ordinary 44.1k session.
		flt::gainProfile(fs, host_.sampleRate(), freq.data(), gain.data(), kCurvePoints);
		canvas.setColor(colour);
		float prevX = box.x();
		float prevY = box.bottom();
		for (size_t i = 0; i < kCurvePoints; ++i) {
			const float x =
			    box.x() + box.width() * (static_cast<float>(i) / static_cast<float>(kCurvePoints - 1));
			const float db = 20.0f * std::log10(std::max(gain[i], 1e-6f));
			const float ty = std::clamp((12.0f - db) / 48.0f, 0.0f, 1.0f);
			const float y = box.y() + ty * box.height();
			if (i > 0) {
				canvas.segment(prevX, prevY, x, y, thickness, true);
			}
			prevX = x;
			prevY = y;
		}
	};

	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		flt::Settings fs;
		fs.enabled = true;
		fs.type = s.type;
		fs.cutoffLog2Hz = s.point[i].cutoffLog2Hz;
		fs.resonance = s.point[i].resonance;
		fs.macro1 = s.point[i].macro1;
		fs.macro2 = s.point[i].macro2;
		drawCurve(fs, withAlpha(kLine, 0.35f), 1.0f);
	}
	drawCurve(wp::settingsAtMorph(s, static_cast<double>(s.morph)), withAlpha(kAccent, 0.9f), 2.0f);
}

std::vector<wp::Param> WaypointExpanded::addressedParamsForTest() const {
	// ASKED OF THE KNOBS THIS PANEL ACTUALLY BUILT, not restated. A list written by hand beside the
	// constructor is a list that drifts from it. The three choices that are not knobs are added by name.
	std::vector<wp::Param> out = knobParams_;
	out.push_back(wp::Param::Type);
	out.push_back(wp::Param::Keytrack);
	out.push_back(wp::Param::Poly);
	return out;
}

} // namespace feathers::ui
