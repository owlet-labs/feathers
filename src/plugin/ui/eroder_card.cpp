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

#include "plugin/ui/control_face.h"

#include <algorithm>
#include <functional>

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
	bias_ = knob(ero::Param::RustBias, "Bias");
	freq_ = knob(ero::Param::Freq, "Freq");
	width_ = knob(ero::Param::Width, "Width");
	mix_ = knob(ero::Param::Mix, "Mix");
	cutoff_ = knob(ero::Param::Cutoff, "Cutoff");
	emphasis_ = knob(ero::Param::EmphAmount, "Emph");
	resonance_ = knob(ero::Param::Resonance, "Reso");
	loop_ = knob(ero::Param::Loop, "Loop");

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
	// ETCH'S SMALL ROW: Freq and Mix as compact boxes (they are boxes on the knob row's place, which is the slots'). A box
	// is a modulation target and takes a drag as a knob does (`ValueBox`), so neither is lost to the card.
	freqBox_ = box(ero::Param::Freq, "Freq");
	mixBox_ = box(ero::Param::Mix, "Mix");
	// ETCH'S SLOT ROW, bare: a caption band would not fit the card's height.
	etch_.build([this](ero::Param p) -> DeviceControl& { return control(p); }, kKnobDiameter, notifier_, "");
	addChild(etch_.outer.get());
	track_ = std::make_unique<SwitchChip>(control(ero::Param::Track), "Track");
	addChild(track_.get());
	// THE COMB'S TWO CHIPS: Tap (on is Driven) and Restore.
	tap_ = std::make_unique<SwitchChip>(control(ero::Param::FbTap), "Driven");
	addChild(tap_.get());
	restore_ = std::make_unique<SwitchChip>(control(ero::Param::EmphRestore), "Restore");
	addChild(restore_.get());

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
	etch_.setModulation(mod, history);
	// THE KNOBS DRAW THEIR OWN ARM GLOW, and watch the panel state for it (`CompCard::setModulation`).
	for (Knob* knob : { amount_.get(), bias_.get(), freq_.get(), width_.get(), mix_.get(), cutoff_.get(),
	                    drive_.get(), emphasis_.get(), resonance_.get(), loop_.get() }) {
		knob->setModulation(mod, history);
	}
}

ero::Mode EroderCard::engine() const {
	const int at = static_cast<int>(host_.fieldAt(device_, static_cast<size_t>(ero::Param::Mode)) + 0.5f);
	return static_cast<ero::Mode>(std::clamp(at, 0, static_cast<int>(ero::Mode::kCount) - 1));
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
	return { amount_.get() };
}

std::vector<visage::Frame*> EroderCard::rustWidgetsForTest() const { return { bias_.get() }; }

std::vector<visage::Frame*> EroderCard::shakerWidgetsForTest() const {
	return { freq_.get(), width_.get(), source_.get(), grit_.get(), track_.get() };
}

std::vector<visage::Frame*> EroderCard::etchWidgetsForTest() const {
	return { etch_.outer.get(), freqBox_.get(), mixBox_.get(), track_.get() };
}

std::vector<visage::Frame*> EroderCard::combWidgetsForTest() const {
	return { tone_.get(), character_.get(), cutoff_.get(), gamma_.get(), tap_.get(), restore_.get(), loop_.get() };
}

void EroderCard::resized() { layOut(); }

void EroderCard::foldedChanged() { layOut(); }

void EroderCard::layOut() {
	const ero::Mode mode = engine();
	laidOutEngine_ = static_cast<int>(mode);
	const bool comb = mode == ero::Mode::Comb;
	const bool etch = mode == ero::Mode::Etch;
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
	// THE FIRST CELL IS THE ENGINE'S OWN DEPTH: Dust's Amount, Rust's Bias, the Comb's Tone.
	amount_->setBounds(left, y, cell, knobH);
	amount_->setVisible(open && mode == ero::Mode::Dust);
	bias_->setBounds(left, y, cell, knobH);
	bias_->setVisible(open && mode == ero::Mode::Rust);
	visage::Frame* const shakerRow[] = { freq_.get(), width_.get() };
	visage::Frame* const combRow[] = { tone_.get(), character_.get(), cutoff_.get() };
	for (size_t i = 0; i < 2; ++i) {
		shakerRow[i]->setBounds(left + cell * static_cast<float>(i + 1), y, cell, knobH);
		shakerRow[i]->setVisible(open && !comb && !etch);
	}
	for (size_t i = 0; i < 3; ++i) {
		combRow[i]->setBounds(left + cell * static_cast<float>(i), y, cell, knobH);
		combRow[i]->setVisible(open && comb);
	}
	mix_->setBounds(left + cell * 3.0f, y, cell, knobH);
	mix_->setVisible(open && !etch);
	// ETCH'S KNOB ROW IS THE SLOTS: A, its Type with Shape and Mix, the route, B (one line, the card's four cells).
	etch_.outer->setBounds(left, y, 4.0f * cell, etch_.heightFor(knobH));
	etch_.setShown(open && etch);
	y += knobH + kRowGap;

	// THE SMALL ROW. Dust: Source, Grit, Track. Comb: Gamma.
	const float gap = 5.0f;
	const float chipW = 44.0f;
	const float boxW = std::max(0.0f, (width - chipW - 2.0f * gap) * 0.5f);
	const float boxY = y + (kSmallRowHeight - ValueBox::kCompactHeight) * 0.5f;
	source_->setBounds(left, boxY, boxW, ValueBox::kCompactHeight);
	grit_->setBounds(left + boxW + gap, boxY, boxW, ValueBox::kCompactHeight);
	track_->setBounds(left + width - chipW, y, chipW, SwitchChip::kHeight);
	// ETCH HAS NO SHAKE: Source and Grit are not drawn (a control only the other engines could enable is hidden).
	source_->setVisible(open && !comb && !etch);
	grit_->setVisible(open && !comb && !etch);
	freqBox_->setBounds(left, boxY, boxW, ValueBox::kCompactHeight);
	mixBox_->setBounds(left + boxW + gap, boxY, boxW, ValueBox::kCompactHeight);
	freqBox_->setVisible(open && etch);
	mixBox_->setVisible(open && etch);
	track_->setVisible(open && !comb);
	// THE COMB'S SMALL ROW: Gamma, then Tap (Driven) and Restore.
	const float restoreW = 52.0f;
	const float tapW = 48.0f;
	const float gammaW = std::max(0.0f, width - restoreW - tapW - 2.0f * gap);
	gamma_->setBounds(left, boxY, gammaW, ValueBox::kCompactHeight);
	gamma_->setVisible(open && comb);
	tap_->setBounds(left + gammaW + gap, y, tapW, SwitchChip::kHeight);
	tap_->setVisible(open && comb);
	restore_->setBounds(left + width - restoreW, y, restoreW, SwitchChip::kHeight);
	restore_->setVisible(open && comb);
	y += kSmallRowHeight + kRowGap;

	// THE THIRD ROW, every engine's: the Drive pair across two cells, Emphasis, and the feedback (Resonance, or the Comb's
	// outer Loop). HP Wet and Follow are the window's.
	const float under = y + PairCell::kCaptionHeight;
	drivePair_->setBounds(left, y, 2.0f * cell, PairCell::heightFor(knobH));
	drivePair_->setVisible(open);
	emphasis_->setBounds(left + cell * 2.0f, under, cell, knobH);
	emphasis_->setVisible(open);
	resonance_->setBounds(left + cell * 3.0f, under, cell, knobH);
	resonance_->setVisible(open && !comb);
	loop_->setBounds(left + cell * 3.0f, under, cell, knobH);
	loop_->setVisible(open && comb);
	refreshHonesty();
}

void EroderCard::refreshHonesty() {
	// WIDTH IS THE NOISE BAND'S: with the shake all sine there is no band, so it is drawn out of the signal (and
	// still takes the hand — it is a setting the engine reads the moment Source comes up).
	ero::Row row {};
	(void)readModulatedRow(host_, device_, ero::kDescriptor, row);
	// ... OR THE EMPHASIS IS A BELL, whose width Width is.
	const bool emphasised = row[static_cast<size_t>(ero::Param::EmphAmount)] != 0.0f;
	const bool bell = row[static_cast<size_t>(ero::Param::EmphShape)] >= 0.5f;
	// ETCH HAS NO NOISE BAND: Source is not its business, the Bell's width is.
	const bool etch = engine() == ero::Mode::Etch;
	width_->setActive((!etch && row[static_cast<size_t>(ero::Param::Source)] > 0.0f) || (bell && emphasised));
	// RESTORE PUTS BACK WHAT THE EMPHASIS TOOK: with none there is nothing to put back. (The Tap is always live in the
	// Comb: its inner loop never stops, `kBankFloor`.)
	restore_->setEnabled(emphasised);
	(void)driveCell_->refresh();
}

bool EroderCard::updateContent() {
	bool changed = false;
	// THE ENGINE MOVED (a hand, automation, undo, a preset): the other engine's controls show.
	if (static_cast<int>(engine()) != laidOutEngine_) {
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

std::vector<ero::Param> EroderCard::addressedParamsForTest() const {
	// **READ OFF THE WIDGET TREE** (`ControlFace`), hidden ones and every engine's included: each frame says which controls it
	// offers (the plot's handles by their gesture, the slot row's cells by their face) and the answer is mapped to the
	// parameter by the control the face was built over. Nothing here lists a parameter.
	std::vector<ero::Param> out;
	std::vector<ControlFace::Offered> offered;
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ControlFace*>(frame)) {
			offered.clear();
			face->offeredControls(offered);
			for (const ControlFace::Offered& o : offered) {
				for (size_t i = 0; i < controls_.size(); ++i) {
					const auto p = static_cast<ero::Param>(i);
					if (controls_[i] != nullptr && static_cast<const device::Modulatable*>(controls_[i].get()) == o.control
					    && std::find(out.begin(), out.end(), p) == out.end()) {
						out.push_back(p);
					}
				}
			}
		}
		for (const visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(this);
	return out;
}

} // namespace feathers::ui
