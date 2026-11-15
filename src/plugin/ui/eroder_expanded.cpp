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


#include "plugin/ui/eroder_expanded.h"

#include "plugin/ui/generic_card.h"
#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <span>

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

struct KnobSpec {
	ero::Param param;
	const char* caption;
};
/// EVERY PLAIN KNOB THE WINDOW HAS. Which are drawn, and in what order, is the engine's (`layOut`).
constexpr KnobSpec kKnobs[] = {
	{ ero::Param::Amount, "Amount" },   { ero::Param::Freq, "Freq" },         { ero::Param::Width, "Width" },
	{ ero::Param::Source, "Source" },   { ero::Param::Grit, "Grit" },         { ero::Param::Stereo, "Stereo" },
	{ ero::Param::Resonance, "Reso" },  { ero::Param::Follow, "Follow" },     { ero::Param::Mix, "Mix" },
	{ ero::Param::HpWet, "HP Wet" },    { ero::Param::Step, "Step" },         { ero::Param::Detune, "Detune" },
	{ ero::Param::ModFold, "Mod Fold" }, { ero::Param::RustBias, "Bias" },    { ero::Param::RustRectify, "Rectify" },
	{ ero::Param::Cutoff, "Cutoff" },   { ero::Param::Heat, "Heat" },
};

using P = ero::Param;
/// THE GRID, an engine at a time, three across. The Drive pair and HP Wet follow in every one.
constexpr P kDustGrid[] = { P::Amount, P::Freq, P::Width, P::Source, P::Grit, P::Stereo, P::Resonance, P::Follow, P::Mix };
constexpr P kRustGrid[] = { P::RustBias, P::RustRectify, P::Freq, P::Width, P::Source, P::Grit, P::Stereo, P::Follow, P::Mix };
/// Tone and Character lead the Comb's and are coordinate cells, not knobs.
constexpr P kCombGrid[] = { P::Cutoff, P::Heat, P::Follow, P::Mix };
/// THE SHAKE'S EXTRAS, a row of their own under the pair: Dust's and Rust's.
constexpr P kShakeRow[] = { P::Step, P::Detune, P::ModFold };

} // namespace

EroderExpanded::EroderExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : dhost_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(ero::Param::kCount));
	plot_ = std::make_unique<EroderPlot>(host, id, true);
	addChild(plot_.get());
	mode_ = std::make_unique<SegmentedControl>(control(ero::Param::Mode), registry, ero::kModeNames, notifier_);
	addChild(mode_.get());

	for (const KnobSpec& spec : kKnobs) {
		auto k = std::make_unique<Knob>(control(spec.param), spec.caption, notifier_);
		k->setFaceDiameter(kFace);
		addChild(k.get());
		knobs_.emplace_back(spec.param, std::move(k));
	}
	const auto chip = [this](ero::Param p, const char* caption) {
		auto c = std::make_unique<SwitchChip>(control(p), caption);
		addChild(c.get());
		return c;
	};
	track_ = chip(ero::Param::Track, "Track");
	subThru_ = chip(ero::Param::SubThru, "Sub Thru");
	blend_ = chip(ero::Param::Blend, "Blend");

	// DRIVE, A PAIR (`EroderCard`'s constructor).
	drive_ = std::make_unique<Knob>(control(ero::Param::Drive), "Amount", notifier_);
	drive_->setFaceDiameter(kFace);
	std::vector<ZoneSelect::ZoneName> types;
	for (const char* name : ero::kDriveModeNames) {
		types.push_back({ name, {} });
	}
	driveCell_ = std::make_unique<ModeValueCell>(control(ero::Param::DriveMode), "Type", std::move(types),
	                                             [](size_t) { return true; });
	driveCell_->setFaceDiameter(kFace);
	(void)driveCell_->addFollower(static_cast<size_t>(ero::Param::DriveShape),
	                              std::make_unique<ValueBox>(control(ero::Param::DriveShape), ""));
	(void)driveCell_->refresh();
	drivePair_ = std::make_unique<PairCell>("Drive", *drive_, *driveCell_);
	addChild(drivePair_.get());

	const auto coordinate = [this](ero::Param p, const char* caption) {
		const device::ParamDesc& d = ero::kParamDescs[static_cast<size_t>(p)];
		auto cell = std::make_unique<ZoneCoordCell>(control(p), caption, GenericCard::zoneNamesFor(d),
		                                            ZoneSelect::kCoordinatePerLap);
		cell->setFaceDiameter(kFace);
		addChild(cell.get());
		return cell;
	};
	tone_ = coordinate(ero::Param::Tone, "Tone");
	character_ = coordinate(ero::Param::Character, "Character");
	gamma_ = std::make_unique<GammaBox>(control(ero::Param::Gamma), "Gamma");
	addChild(gamma_.get());
}

DeviceControl& EroderExpanded::control(ero::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(dhost_, device_, ero::kDescriptor, i);
	}
	return *controls_[i];
}

Knob* EroderExpanded::knob(ero::Param p) const {
	for (const auto& [param, k] : knobs_) {
		if (param == p) {
			return k.get();
		}
	}
	return nullptr;
}

void EroderExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < static_cast<size_t>(ero::Param::kCount); ++i) {
		control(static_cast<ero::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
	// THE KNOBS DRAW THEIR OWN ARM GLOW, and watch the panel state for it (`CompExpanded::modulationBound`).
	for (const auto& entry : knobs_) {
		entry.second->setModulation(mod, history);
	}
	drive_->setModulation(mod, history);
}

std::string EroderExpanded::title() const { return ero::kTraits.name; }

visage::Point EroderExpanded::minimumSize() const {
	// THE TALLEST ENGINE'S COLUMN: three rows of knobs, the pair, the shake's row, the chips.
	const float knobH = Knob::columnHeight(kFace);
	const float side = kSwitchHeight + kGap + 4.0f * (knobH + kGap) + PairCell::heightFor(knobH) + kGap
	                   + SwitchChip::kHeight + 2.0f * kPad;
	return { 700.0f, std::max(380.0f, side) };
}

ero::Mode EroderExpanded::engine() const {
	const int at = static_cast<int>(dhost_.fieldAt(device_, static_cast<size_t>(ero::Param::Mode)) + 0.5f);
	return static_cast<ero::Mode>(std::clamp(at, 0, static_cast<int>(ero::Mode::kCount) - 1));
}

std::vector<visage::Frame*> EroderExpanded::dustWidgetsForTest() const {
	return { knob(P::Amount), knob(P::Resonance) };
}

std::vector<visage::Frame*> EroderExpanded::combWidgetsForTest() const {
	return { tone_.get(), character_.get(), knob(P::Cutoff), knob(P::Heat), gamma_.get(), blend_.get() };
}

std::vector<visage::Frame*> EroderExpanded::rustWidgetsForTest() const {
	return { knob(P::RustBias), knob(P::RustRectify) };
}

void EroderExpanded::resized() { layOut(); }

void EroderExpanded::layOut() {
	const ero::Mode mode = engine();
	laidOutEngine_ = static_cast<int>(mode);
	const bool comb = mode == ero::Mode::Comb;
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - kSideWidth - kGap);
	plot_->setBounds(kPad, kPad, plotW, std::max(0.0f, height() - 2.0f * kPad));

	const float x = width() - kPad - kSideWidth;
	float y = kPad;
	mode_->setBounds(x, y, kSideWidth, kSwitchHeight);
	y += kSwitchHeight + kGap;

	const float cell = kSideWidth / static_cast<float>(kPerRow);
	const float knobH = Knob::columnHeight(kFace);
	// EVERYTHING IS HIDDEN, THEN THE ENGINE'S OWN ARE FLOWED: a hidden item takes no room, and the Drive pair wraps
	// whole (`flowCells`).
	for (const auto& entry : knobs_) {
		entry.second->setVisible(false);
	}
	std::vector<FlowItem> items;
	items.push_back({ tone_.get(), 1, comb, knobH });
	items.push_back({ character_.get(), 1, comb, knobH });
	const auto add = [&](std::span<const P> grid) {
		for (const P p : grid) {
			items.push_back({ knob(p), 1, true, knobH });
		}
	};
	add(mode == ero::Mode::Dust ? std::span<const P>(kDustGrid)
	                            : comb ? std::span<const P>(kCombGrid) : std::span<const P>(kRustGrid));
	items.push_back({ drivePair_.get(), 2, true, PairCell::heightFor(knobH) });
	items.push_back({ knob(P::HpWet), 1, true, PairCell::heightFor(knobH) });
	if (!comb) {
		add(kShakeRow);
	}
	const float under = flowCells(items, static_cast<int>(kPerRow), x, y, cell, kGap) + kGap;
	// HP WET SITS BESIDE THE PAIR, level with the cells inside it.
	Knob* hp = knob(P::HpWet);
	hp->setBounds(hp->x(), drivePair_->y() + PairCell::kCaptionHeight, cell, knobH);

	// THE CHIPS: Track (the shakers') or Blend (the Comb's), then Sub Thru; the Comb's Gamma under them.
	track_->setBounds(x, under, 64.0f, SwitchChip::kHeight);
	track_->setVisible(!comb);
	blend_->setBounds(x, under, 64.0f, SwitchChip::kHeight);
	blend_->setVisible(comb);
	subThru_->setBounds(x + 2.0f * cell, under, cell, SwitchChip::kHeight);
	subThru_->setVisible(true);
	gamma_->setBounds(x, under + SwitchChip::kHeight + kGap, 2.0f * cell, ValueBox::kCompactHeight);
	gamma_->setVisible(comb);
	refreshHonesty();
}

void EroderExpanded::refreshHonesty() {
	ero::Row row {};
	(void)readModulatedRow(dhost_, device_, ero::kDescriptor, row);
	const float source = row[static_cast<size_t>(ero::Param::Source)];
	// WIDTH IS THE NOISE BAND'S (`EroderCard::refreshHonesty`); Detune and Mod Fold are the sine's.
	knob(P::Width)->setActive(source > 0.0f);
	knob(P::Detune)->setActive(source < 1.0f);
	knob(P::ModFold)->setActive(source < 1.0f);
	// SUB THRU GIVES BACK WHAT HP WET TOOK OF A DRY THAT WAS TURNED DOWN: with no filter there is nothing to give
	// back, and in the Comb the dry is whole unless Blend crossfades it.
	const bool filtering = static_cast<double>(row[static_cast<size_t>(ero::Param::HpWet)]) > ero::kHpWetLog2Min;
	const bool crossfades = engine() != ero::Mode::Comb || row[static_cast<size_t>(ero::Param::Blend)] >= 0.5f;
	subThru_->setEnabled(filtering && crossfades);
	(void)driveCell_->refresh();
}

bool EroderExpanded::updateContent() {
	bool changed = false;
	if (static_cast<int>(engine()) != laidOutEngine_) {
		layOut();
		changed = true;
	}
	if (plot_->update()) {
		changed = true;
	}
	refreshHonesty();
	if (changed) {
		redraw();
	}
	return changed;
}

void EroderExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width(), height());
}

} // namespace feathers::ui
