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

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

struct KnobSpec {
	ero::Param param;
	const char* caption;
};
/// DUST'S PLAIN KNOBS. Follow and Mix follow them (the eighth and ninth cells, the Comb's fifth and fourth), then
/// the Drive pair and HP Wet.
constexpr std::array<KnobSpec, 7> kDustKnobs { {
	{ ero::Param::Amount, "Amount" },
	{ ero::Param::Freq, "Freq" },
	{ ero::Param::Width, "Width" },
	{ ero::Param::Source, "Source" },
	{ ero::Param::Grit, "Grit" },
	{ ero::Param::Stereo, "Stereo" },
	{ ero::Param::Resonance, "Reso" },
} };

} // namespace

EroderExpanded::EroderExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : dhost_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(ero::Param::kCount));
	plot_ = std::make_unique<EroderPlot>(host, id, true);
	addChild(plot_.get());
	mode_ = std::make_unique<SegmentedControl>(control(ero::Param::Mode), registry, ero::kModeNames, notifier_);
	addChild(mode_.get());

	const auto knob = [this](ero::Param p, const char* caption) {
		auto k = std::make_unique<Knob>(control(p), caption, notifier_);
		k->setFaceDiameter(kFace);
		addChild(k.get());
		return k;
	};
	for (const KnobSpec& spec : kDustKnobs) {
		dustKnobs_.push_back(knob(spec.param, spec.caption));
	}
	mix_ = knob(ero::Param::Mix, "Mix");
	cutoff_ = knob(ero::Param::Cutoff, "Cutoff");
	follow_ = knob(ero::Param::Follow, "Follow");
	hpWet_ = knob(ero::Param::HpWet, "HP Wet");
	subThru_ = std::make_unique<SwitchChip>(control(ero::Param::SubThru), "Sub Thru");
	addChild(subThru_.get());

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
	track_ = std::make_unique<SwitchChip>(control(ero::Param::Track), "Track");
	addChild(track_.get());

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

void EroderExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < static_cast<size_t>(ero::Param::kCount); ++i) {
		control(static_cast<ero::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
	// THE KNOBS DRAW THEIR OWN ARM GLOW, and watch the panel state for it (`CompExpanded::modulationBound`).
	for (auto& knob : dustKnobs_) {
		knob->setModulation(mod, history);
	}
	for (Knob* knob : { mix_.get(), cutoff_.get(), follow_.get(), drive_.get(), hpWet_.get() }) {
		knob->setModulation(mod, history);
	}
}

std::string EroderExpanded::title() const { return ero::kTraits.name; }

visage::Point EroderExpanded::minimumSize() const {
	const float side = kSwitchHeight + kGap + 3.0f * (Knob::columnHeight(kFace) + kGap)
	                   + PairCell::heightFor(Knob::columnHeight(kFace)) + kGap + SwitchChip::kHeight + 2.0f * kPad;
	return { 700.0f, std::max(380.0f, side) };
}

bool EroderExpanded::dust() const {
	return static_cast<int>(dhost_.fieldAt(device_, static_cast<size_t>(ero::Param::Mode)) + 0.5f)
	       == static_cast<int>(ero::Mode::Dust);
}

std::vector<visage::Frame*> EroderExpanded::dustWidgetsForTest() const {
	std::vector<visage::Frame*> out;
	for (const auto& k : dustKnobs_) {
		out.push_back(k.get());
	}
	out.push_back(hpWet_.get());
	out.push_back(track_.get());
	out.push_back(subThru_.get());
	return out;
}

std::vector<visage::Frame*> EroderExpanded::combWidgetsForTest() const {
	return { tone_.get(), character_.get(), cutoff_.get(), gamma_.get() };
}

void EroderExpanded::resized() { layOut(); }

void EroderExpanded::layOut() {
	const bool isDust = dust();
	laidOutDust_ = isDust ? 1 : 0;
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - kSideWidth - kGap);
	plot_->setBounds(kPad, kPad, plotW, std::max(0.0f, height() - 2.0f * kPad));

	const float x = width() - kPad - kSideWidth;
	float y = kPad;
	mode_->setBounds(x, y, kSideWidth, kSwitchHeight);
	y += kSwitchHeight + kGap;

	const float cell = kSideWidth / static_cast<float>(kPerRow);
	const float knobH = Knob::columnHeight(kFace);
	// ONE FLOW FOR BOTH ENGINES: a hidden item takes no room, and the Drive pair wraps whole (`flowCells`).
	std::vector<FlowItem> items;
	for (const auto& knob : dustKnobs_) {
		items.push_back({ knob.get(), 1, isDust, knobH });
	}
	items.push_back({ tone_.get(), 1, !isDust, knobH });
	items.push_back({ character_.get(), 1, !isDust, knobH });
	items.push_back({ cutoff_.get(), 1, !isDust, knobH });
	// DUST: Follow then Mix close the third row. COMB: Mix then Follow open the second.
	items.push_back({ isDust ? follow_.get() : mix_.get(), 1, true, knobH });
	items.push_back({ isDust ? mix_.get() : follow_.get(), 1, true, knobH });
	// THE DRIVE PAIR IS BOTH ENGINES'. In the Comb it follows Follow on the second row; in Dust it has a row.
	items.push_back({ drivePair_.get(), 2, true, PairCell::heightFor(knobH) });
	const float under = flowCells(items, static_cast<int>(kPerRow), x, y, cell, kGap) + kGap;
	// HP WET SITS BESIDE THE PAIR, level with the cells inside it.
	hpWet_->setBounds(x + 2.0f * cell, drivePair_->y() + PairCell::kCaptionHeight, cell, knobH);
	hpWet_->setVisible(isDust);

	track_->setBounds(x, under, 64.0f, SwitchChip::kHeight);
	track_->setVisible(isDust);
	subThru_->setBounds(x + 2.0f * cell, under, cell, SwitchChip::kHeight);
	subThru_->setVisible(isDust);
	gamma_->setBounds(x, under, 2.0f * cell, ValueBox::kCompactHeight);
	gamma_->setVisible(!isDust);
	refreshHonesty();
}

Knob* EroderExpanded::widthForTest() const { return dustKnobs_[2].get(); }

void EroderExpanded::refreshHonesty() {
	ero::Row row {};
	(void)readModulatedRow(dhost_, device_, ero::kDescriptor, row);
	// WIDTH IS THE NOISE BAND'S (`EroderCard::refreshHonesty`).
	dustKnobs_[2]->setActive(row[static_cast<size_t>(ero::Param::Source)] > 0.0f);
	// SUB THRU GIVES BACK WHAT HP WET TOOK: with no filter there is nothing to give back.
	subThru_->setEnabled(static_cast<double>(row[static_cast<size_t>(ero::Param::HpWet)]) > ero::kHpWetLog2Min);
	(void)driveCell_->refresh();
}

bool EroderExpanded::updateContent() {
	bool changed = false;
	if ((dust() ? 1 : 0) != laidOutDust_) {
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
