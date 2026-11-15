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
/// DUST'S GRID, Mix excepted (it is the ninth cell and the Comb's fourth).
constexpr std::array<KnobSpec, 8> kDustKnobs { {
	{ ero::Param::Amount, "Amount" },
	{ ero::Param::Freq, "Freq" },
	{ ero::Param::Width, "Width" },
	{ ero::Param::Source, "Source" },
	{ ero::Param::Grit, "Grit" },
	{ ero::Param::Stereo, "Stereo" },
	{ ero::Param::Resonance, "Reso" },
	{ ero::Param::Drive, "Drive" },
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
	mix_->setModulation(mod, history);
	cutoff_->setModulation(mod, history);
}

std::string EroderExpanded::title() const { return ero::kTraits.name; }

visage::Point EroderExpanded::minimumSize() const {
	const float side = kSwitchHeight + kGap + 3.0f * (Knob::columnHeight(kFace) + kGap) + SwitchChip::kHeight
	                   + 2.0f * kPad;
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
	out.push_back(track_.get());
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
	const auto place = [&](visage::Frame* frame, size_t index, bool shown) {
		const auto row = static_cast<float>(index / kPerRow);
		const auto column = static_cast<float>(index % kPerRow);
		frame->setBounds(x + column * cell, y + row * (knobH + kGap), cell, knobH);
		frame->setVisible(shown);
	};
	for (size_t i = 0; i < dustKnobs_.size(); ++i) {
		place(dustKnobs_[i].get(), i, isDust);
	}
	place(tone_.get(), 0, !isDust);
	place(character_.get(), 1, !isDust);
	place(cutoff_.get(), 2, !isDust);
	// MIX: the ninth cell of Dust's grid, the fourth of the Comb's.
	place(mix_.get(), isDust ? dustKnobs_.size() : 3, true);

	const float under = y + (isDust ? 3.0f : 2.0f) * (knobH + kGap);
	track_->setBounds(x, under, 64.0f, SwitchChip::kHeight);
	track_->setVisible(isDust);
	gamma_->setBounds(x + cell, y + knobH + kGap + (knobH - ValueBox::kCompactHeight) * 0.5f, 2.0f * cell,
	                  ValueBox::kCompactHeight);
	gamma_->setVisible(!isDust);
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
