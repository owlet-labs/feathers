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


#include "plugin/ui/eroder_card.h"

#include "plugin/ui/modulated_row.h"

#include "plugin/ui/generic_card.h"
#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

namespace ero = fx::eroder;

EroderCard::EroderCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(ero::Param::kCount));

	plot_ = std::make_unique<EroderPlot>(host, id, false);
	addChild(plot_.get());
	mode_ = std::make_unique<SegmentedControl>(control(ero::Param::Mode), registry, ero::kModeNames, notifier_);
	addChild(mode_.get());

	const auto knob = [this](ero::Param p, const char* caption) {
		auto k = std::make_unique<Knob>(control(p), caption, notifier_);
		k->setFaceDiameter(kKnobDiameter);
		addChild(k.get());
		return k;
	};
	amount_ = knob(ero::Param::Amount, "Amount");
	freq_ = knob(ero::Param::Freq, "Freq");
	width_ = knob(ero::Param::Width, "Width");
	mix_ = knob(ero::Param::Mix, "Mix");
	cutoff_ = knob(ero::Param::Cutoff, "Cutoff");
	hpWet_ = knob(ero::Param::HpWet, "HP Wet");
	follow_ = knob(ero::Param::Follow, "Follow");

	// DRIVE, A PAIR: the knob, and the Type with its own Shape under it — a Shape stops at its ends and only the
	// selector changes the Type (`mode_value_cell.h`).
	drive_ = std::make_unique<Knob>(control(ero::Param::Drive), "Amount", notifier_);
	drive_->setFaceDiameter(kKnobDiameter);
	std::vector<ZoneSelect::ZoneName> types;
	for (const char* name : ero::kDriveModeNames) {
		types.push_back({ name, {} });
	}
	driveCell_ = std::make_unique<ModeValueCell>(control(ero::Param::DriveMode), "Type", std::move(types),
	                                             [](size_t) { return true; });
	driveCell_->setFaceDiameter(kKnobDiameter);
	(void)driveCell_->addFollower(static_cast<size_t>(ero::Param::DriveShape),
	                              std::make_unique<ValueBox>(control(ero::Param::DriveShape), ""));
	(void)driveCell_->refresh();
	drivePair_ = std::make_unique<PairCell>("Drive", *drive_, *driveCell_);
	addChild(drivePair_.get());

	const auto box = [this](ero::Param p, const char* caption) {
		auto b = std::make_unique<ValueBox>(control(p), caption);
		b->setCompact(true);
		addChild(b.get());
		return b;
	};
	source_ = box(ero::Param::Source, "Source");
	grit_ = box(ero::Param::Grit, "Grit");
	track_ = std::make_unique<SwitchChip>(control(ero::Param::Track), "Track");
	addChild(track_.get());

	// THE COMB'S TWO COORDINATES: zoned, so a coordinate cell each (`GenericCard`'s reason), "Char" because the whole
	// word does not fit a quarter of the card.
	const auto coordinate = [this](ero::Param p, const char* caption) {
		const device::ParamDesc& d = ero::kParamDescs[static_cast<size_t>(p)];
		auto cell = std::make_unique<ZoneCoordCell>(control(p), caption, GenericCard::zoneNamesFor(d),
		                                            ZoneSelect::kCoordinatePerLap);
		cell->setFaceDiameter(kKnobDiameter);
		addChild(cell.get());
		return cell;
	};
	tone_ = coordinate(ero::Param::Tone, "Tone");
	character_ = coordinate(ero::Param::Character, "Char");
	gamma_ = std::make_unique<GammaBox>(control(ero::Param::Gamma), "Gamma");
	addChild(gamma_.get());

	// THE HEAD'S OCCUPANTS — `DisperserCard`'s constructor.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Eroder on" : "Eroder off");
			}
		}
	};
}

DeviceControl& EroderCard::control(ero::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, ero::kDescriptor, i);
	}
	return *controls_[i];
}

device::Modulatable* EroderCard::enableModulatable() { return &control(ero::Param::Enabled); }

bool EroderCard::headEnabled() const { return control(ero::Param::Enabled).value() >= 0.5; }

void EroderCard::setModulation(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < static_cast<size_t>(ero::Param::kCount); ++i) {
		control(static_cast<ero::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
	// THE KNOBS DRAW THEIR OWN ARM GLOW, and watch the panel state for it (`CompCard::setModulation`).
	for (Knob* knob : { amount_.get(), freq_.get(), width_.get(), mix_.get(), cutoff_.get(), drive_.get(),
	                    hpWet_.get(), follow_.get() }) {
		knob->setModulation(mod, history);
	}
}

bool EroderCard::dust() const {
	return static_cast<int>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::Mode)) + 0.5f)
	       == static_cast<int>(ero::Mode::Dust);
}

float EroderCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kPlotHeight + kRowGap + kSwitchHeight + kRowGap + Knob::columnHeight(kKnobDiameter)
	       + kRowGap + kSmallRowHeight + kRowGap + PairCell::heightFor(Knob::columnHeight(kKnobDiameter)) + kPad;
}

float EroderCard::laneHeightAt(float) const { return folded() ? foldedHeight() : openHeight(); }

float EroderCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

std::vector<visage::Frame*> EroderCard::dustWidgetsForTest() const {
	return { amount_.get(), freq_.get(), width_.get(), source_.get(), grit_.get(), track_.get(), hpWet_.get() };
}

std::vector<visage::Frame*> EroderCard::combWidgetsForTest() const {
	return { tone_.get(), character_.get(), cutoff_.get(), gamma_.get() };
}

void EroderCard::resized() { layOut(); }

void EroderCard::foldedChanged() { layOut(); }

void EroderCard::layOut() {
	const bool isDust = dust();
	laidOutDust_ = isDust ? 1 : 0;
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	float y = kSlotHeadHeight + 4.0f;

	// `setBounds` UNCONDITIONALLY throughout (CLAUDE.md's DPI rule).
	plot_->setBounds(left, y, width, kPlotHeight);
	plot_->setVisible(open);
	y += kPlotHeight + kRowGap;

	mode_->setBounds(left, y, width, kSwitchHeight);
	mode_->setVisible(open);
	y += kSwitchHeight + kRowGap;

	// THE KNOB ROW: four cells, Mix the last of them in both engines so it does not move on the switch.
	const float cell = width / 4.0f;
	const float knobH = Knob::columnHeight(kKnobDiameter);
	visage::Frame* const dustRow[] = { amount_.get(), freq_.get(), width_.get() };
	visage::Frame* const combRow[] = { tone_.get(), character_.get(), cutoff_.get() };
	for (size_t i = 0; i < 3; ++i) {
		const float x = left + cell * static_cast<float>(i);
		dustRow[i]->setBounds(x, y, cell, knobH);
		dustRow[i]->setVisible(open && isDust);
		combRow[i]->setBounds(x, y, cell, knobH);
		combRow[i]->setVisible(open && !isDust);
	}
	mix_->setBounds(left + cell * 3.0f, y, cell, knobH);
	mix_->setVisible(open);
	y += knobH + kRowGap;

	// THE SMALL ROW. Dust: Source, Grit, Track. Comb: Gamma.
	const float gap = 5.0f;
	const float chipW = 44.0f;
	const float boxW = std::max(0.0f, (width - chipW - 2.0f * gap) * 0.5f);
	const float boxY = y + (kSmallRowHeight - ValueBox::kCompactHeight) * 0.5f;
	source_->setBounds(left, boxY, boxW, ValueBox::kCompactHeight);
	grit_->setBounds(left + boxW + gap, boxY, boxW, ValueBox::kCompactHeight);
	track_->setBounds(left + width - chipW, y, chipW, SwitchChip::kHeight);
	source_->setVisible(open && isDust);
	grit_->setVisible(open && isDust);
	track_->setVisible(open && isDust);
	gamma_->setBounds(left, boxY, width * 0.5f, ValueBox::kCompactHeight);
	gamma_->setVisible(open && !isDust);
	y += kSmallRowHeight + kRowGap;

	// THE THIRD ROW, both engines': the Drive pair across two cells, and Follow in the fourth. Dust's HP Wet is
	// between them.
	const float under = y + PairCell::kCaptionHeight;
	drivePair_->setBounds(left, y, 2.0f * cell, PairCell::heightFor(knobH));
	drivePair_->setVisible(open);
	hpWet_->setBounds(left + cell * 2.0f, under, cell, knobH);
	hpWet_->setVisible(open && isDust);
	follow_->setBounds(left + cell * 3.0f, under, cell, knobH);
	follow_->setVisible(open);
	refreshHonesty();
}

void EroderCard::refreshHonesty() {
	// WIDTH IS THE NOISE BAND'S: with the shake all sine there is no band, so it is drawn out of the signal (and
	// still takes the hand — it is a setting the engine reads the moment Source comes up).
	ero::Row row {};
	(void)readModulatedRow(host_, device_, ero::kDescriptor, row);
	width_->setActive(row[static_cast<size_t>(ero::Param::Source)] > 0.0f);
	(void)driveCell_->refresh();
}

bool EroderCard::updateContent() {
	bool changed = false;
	// THE ENGINE MOVED (a hand, automation, undo, a preset): the other engine's controls show.
	if ((dust() ? 1 : 0) != laidOutDust_) {
		layOut();
		changed = true;
	}
	if (!folded() && plot_->update()) {
		changed = true;
	}
	refreshHonesty();
	if (changed) {
		redraw();
	}
	return changed;
}

void EroderCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

void EroderCard::drawContent(visage::Canvas&) {}

} // namespace feathers::ui
