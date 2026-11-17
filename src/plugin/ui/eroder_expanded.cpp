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

#include "plugin/ui/control_face.h"

#include <algorithm>
#include <functional>
#include <array>
#include <cctype>
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
	{ ero::Param::Amount, "Amount" },   { ero::Param::Source, "Source" },     { ero::Param::Grit, "Grit" },
	{ ero::Param::Stereo, "Stereo" },
	{ ero::Param::Resonance, "Reso" },  { ero::Param::Follow, "Follow" },     { ero::Param::Mix, "Mix" },
	{ ero::Param::HpWet, "HP Wet" },    { ero::Param::Step, "Step" },         { ero::Param::Detune, "Detune" },
	{ ero::Param::ModFold, "Mod Fold" }, { ero::Param::RustBias, "Bias" },    { ero::Param::RustRectify, "Rectify" },
	{ ero::Param::Cutoff, "Cutoff" },   { ero::Param::Heat, "Heat" },
	{ ero::Param::Smear, "Smear" },     { ero::Param::Loop, "Loop" },
	{ ero::Param::LoopInterval, "Interval" }, { ero::Param::Input, "Input" },
};

using P = ero::Param;
/// THE SHAKE'S EXTRAS, after the engine's own cells: Dust's and Rust's.
constexpr P kShakeRow[] = { P::Step, P::Detune, P::ModFold };
/// THE ENGINE GROUP'S CELLS: the grids above without what the Feedback and Output groups hold.
constexpr P kDustEngine[] = { P::Amount, P::Source, P::Grit, P::Stereo };
constexpr P kRustEngine[] = { P::RustBias, P::RustRectify, P::Source, P::Grit, P::Stereo };
constexpr P kCombEngine[] = { P::Cutoff, P::Heat };

} // namespace

// THE WINDOW'S SLOT BLOCK: slot A's row over slot B's (the top-down order the route switch's glyphs and the topology picture draw),
// the route switch to the right of both, centred on them under its caption.
void SlotStack::resized() {
	const float rowW = std::max(0.0f, static_cast<float>(width()) - kRouteColumn);
	const float rowH = (static_cast<float>(height()) - kRowGap) * 0.5f;
	a_.setBounds(0.0f, 0.0f, rowW, rowH);
	b_.setBounds(0.0f, rowH + kRowGap, rowW, rowH);
	const float switchH = std::min(RouteSwitch::heightFor(44.0f), rowH);
	route_.setBounds(rowW + (kRouteColumn - RouteSwitch::kWidth) * 0.5f,
	                 (static_cast<float>(height()) - switchH) * 0.5f + kCaptionHeight * 0.5f, RouteSwitch::kWidth, switchH);
}

void SlotStack::draw(visage::Canvas& canvas) {
	const float rowW = std::max(0.0f, static_cast<float>(width()) - kRouteColumn);
	canvas.setColor(kMuted);
	canvas.text("ROUTE", font(kKnobLabelSize), visage::Font::kCenter, rowW, route_.y() - kCaptionHeight - 1.0f, kRouteColumn,
	            kCaptionHeight);
}

EroderExpanded::EroderExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : dhost_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(ero::Param::kCount));
	plot_ = std::make_unique<EroderPlot>(host, id, true);
	addChild(plot_.get());
	sub_ = std::make_unique<EroderSubplot>(host, id, *plot_);
	addChild(sub_.get());
	mode_ = std::make_unique<SegmentedControl>(control(ero::Param::Mode), registry, ero::kModeNames, notifier_);
	addChild(mode_.get());

	for (const KnobSpec& spec : kKnobs) {
		auto k = std::make_unique<Knob>(control(spec.param), spec.caption, notifier_);
		k->setFaceDiameter(kFace);
		addChild(k.get());
		knobs_.emplace_back(spec.param, std::move(k));
	}
	// THE THREE SWITCHES THAT CHANGE THE PICTURE'S TOPOLOGY are seated in the picture (`EroderFlow::seat`), not here.
	const auto chip = [this](ero::Param p, const char* caption, bool seated = false) {
		auto c = std::make_unique<SwitchChip>(control(p), caption);
		if (!seated) {
			addChild(c.get());
		}
		return c;
	};
	track_ = chip(ero::Param::Track, "Track");
	subThru_ = chip(ero::Param::SubThru, "Sub Thru", true);
	blend_ = chip(ero::Param::Blend, "Blend");
	restore_ = chip(ero::Param::EmphRestore, "Restore", true);
	inLoop_ = chip(ero::Param::EmphInLoop, "In Loop", true);
	tap_ = chip(ero::Param::FbTap, "Driven", true);
	flow_ = std::make_unique<EroderFlow>(host, id);
	flow_->seat(*inLoop_, *tap_, *restore_, *subThru_);
	addChild(flow_.get());

	// THE FILTER, A STACK OF TWO: Freq and Width, which the plot's handles also write. THE EMPHASIS, A STACK OF THREE: its Amount,
	// its Offset (semitones from Freq) and, at the foot, the Tilt/Bell selector. Each row is its own control, drag-editable and a
	// modulation target.
	filter_ = std::make_unique<FieldStack>("Filter");
	filter_->setFaceDiameter(kFace);
	(void)filter_->addBox(std::make_unique<ValueBox>(control(ero::Param::Freq), ""), "Freq");
	(void)filter_->addBox(std::make_unique<ValueBox>(control(ero::Param::Width), ""), "Width");
	addChild(filter_.get());
	emphasis_ = std::make_unique<FieldStack>("Emphasis");
	emphasis_->setFaceDiameter(kFace);
	(void)emphasis_->addBox(std::make_unique<ValueBox>(control(ero::Param::EmphAmount), ""), "Amt");
	(void)emphasis_->addBox(std::make_unique<ValueBox>(control(ero::Param::EmphOffset), ""), "Off");
	std::vector<ZoneSelect::ZoneName> shapes;
	for (const char* name : ero::kEmphShapeNames) {
		shapes.push_back({ name, {} });
	}
	(void)emphasis_->setSelector(control(ero::Param::EmphShape), std::move(shapes));
	addChild(emphasis_.get());
	loopPair_ = std::make_unique<PairCell>("", *knob(ero::Param::Loop), *knob(ero::Param::LoopInterval));
	addChild(loopPair_.get());

	// DRIVE, A PAIR (`EroderCard`'s constructor).
	// ETCH'S SLOT ROW, captioned (the window has the room).
	etch_.buildStacked([this](ero::Param p) -> DeviceControl& { return control(p); }, kFace, notifier_);
	addChild(etch_.stack.get());

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
	drivePair_ = std::make_unique<PairCell>("", *drive_, *driveCell_);
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
	etch_.setModulation(mod, history);
}

std::string EroderExpanded::title() const { return ero::kTraits.name; }

visage::Point EroderExpanded::minimumSize() const {
	// THE COLUMN, row by row: the engine switch, the first row (Input, Tone, Drive), the Engine group two knob cells tall in every
	// engine, Feedback beside Output, the diagram, and under it Track.
	const float knobH = Knob::columnHeight(kFace);
	const float engine = 2.0f * knobH + kRowGap;
	const float side = kSwitchHeight + kGroupGap + groupHeight(knobH) + kGroupGap + groupHeight(engine) + kGroupGap
	                   + groupHeight(knobH) + kGroupGap + EroderFlow::kHeight + kGroupGap + SwitchChip::kHeight + 2.0f * kPad;
	return { 2.0f * kPad + kGap + 330.0f + kSideColumn, std::max(380.0f, side) };
}

ero::Mode EroderExpanded::engine() const {
	const int at = static_cast<int>(dhost_.fieldAt(device_, static_cast<size_t>(ero::Param::Mode)) + 0.5f);
	return static_cast<ero::Mode>(std::clamp(at, 0, static_cast<int>(ero::Mode::kCount) - 1));
}

std::vector<visage::Frame*> EroderExpanded::dustWidgetsForTest() const {
	return { knob(P::Amount) };
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
	const bool etch = mode == ero::Mode::Etch;
	const float cell = kSideWidth / static_cast<float>(kPerRow);
	// THE SIDE COLUMN: six cells across. The first row is three group boxes (Input one cell, Tone three, Drive two), each with
	// its own edge room; the rows under it share the same width.
	const float sideW = kSideColumn;
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - sideW - kGap);
	// THE PICTURE IS SPLIT THE SAME IN EVERY ENGINE: the spectrum over the sub-plot, so neither moves when the engine does.
	const float pictureH = std::max(0.0f, height() - 2.0f * kPad);
	const float plotH = std::floor((pictureH - kPlotGap) * kPlotShare);
	plot_->setBounds(kPad, kPad, plotW, plotH);
	sub_->setBounds(kPad, kPad + plotH + kPlotGap, plotW, std::max(0.0f, pictureH - plotH - kPlotGap));
	sub_->resized();

	const float x0 = width() - kPad - sideW;
	float y = kPad;
	mode_->setBounds(x0, y, sideW, kSwitchHeight);
	y += kSwitchHeight + kGroupGap;

	const float knobH = Knob::columnHeight(kFace);
	groups_.clear();
	// EVERYTHING IS HIDDEN, THEN THE ENGINE'S OWN ARE FLOWED: a hidden item takes no room, and a pair wraps whole
	// (`flowCells`).
	for (const auto& entry : knobs_) {
		// THE PAIRS' KNOBS ARE SHOWN BY THEIR PAIR.
		const P p = entry.first;
		if (p != P::Loop && p != P::LoopInterval) {
			entry.second->setVisible(false);
		}
	}
	etch_.setShown(etch);
	// A GROUP: its caption band, its items flowed `columns` across and centred in its box. Returns the content's foot; the
	// row's groups are made as tall as the tallest (`closeRow`).
	const auto group = [&](const char* caption, float boxX, float boxW, int columns, const std::vector<FlowItem>& items) {
		const float cellsX = boxX + (boxW - static_cast<float>(columns) * cell) * 0.5f;
		const float under = flowCells(items, columns, cellsX, y + kGroupCaption, cell, kRowGap);
		groups_.push_back({ caption, boxX, y, boxW, under + kGroupPad - y });
		return under;
	};
	const auto closeRow = [&](size_t first, float least = 0.0f) {
		float tallest = least;
		for (size_t i = first; i < groups_.size(); ++i) {
			tallest = std::max(tallest, groups_[i].h);
		}
		for (size_t i = first; i < groups_.size(); ++i) {
			groups_[i].h = tallest;
		}
		y += tallest + kGroupGap;
	};
	// A KNOB BESIDE A TALLER CELL is centred on its height.
	const auto centred = [&](visage::Frame* f, float height) {
		f->setBounds(f->x() + 3.0f, f->y() + (knobH - height) * 0.5f, f->width() - 6.0f, height);
	};

	// ROW ONE: INPUT (the gain into the wet chain), TONE (the filter, the emphasis, Smear) and DRIVE (the amount and what it
	// pushes into): one, three and two cells.
	{
		const size_t first = groups_.size();
		const float inputW = cell + 2.0f * kGroupPad;
		const float toneW = 3.0f * cell + 2.0f * kGroupPad;
		const float driveW = 2.0f * cell + 2.0f * kGroupPad;
		group("Input", x0, inputW, 1, { { knob(P::Input), 1, true, knobH } });
		group("Tone", x0 + inputW + kGroupGap, toneW, 3,
		      { { filter_.get(), 1, true, knobH }, { emphasis_.get(), 1, true, knobH }, { knob(P::Smear), 1, true, knobH } });
		group("Drive", x0 + inputW + toneW + 2.0f * kGroupGap, driveW, 2, { { drivePair_.get(), 2, true, knobH } });
		closeRow(first);
	}

	// ROW TWO: ENGINE, two knob cells tall in every engine. The engine's own cells flow over two rows (Etch: slot A's row above
	// slot B's, the route switch to the right of both), centred in the group's width.
	{
		const size_t first = groups_.size();
		const float contentH = 2.0f * knobH + kRowGap;
		if (etch) {
			const float w = SlotStack::widthFor(2.0f * cell);
			const float top = y + kGroupCaption;
			etch_.stack->setBounds(x0 + (sideW - w) * 0.5f, top, w, contentH);
			etch_.stack->resized();
			groups_.push_back({ "Engine", x0, y, sideW, kGroupCaption + contentH + kGroupPad });
			// ETCH'S OWN CELLS ARE THE STACK'S; NOTHING ELSE FLOWS HERE.
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { tone_.get(), character_.get(), blend_.get(), gamma_.get() }) {
				f->setVisible(false);
			}
		}
		else {
			std::vector<FlowItem> items;
			items.push_back({ tone_.get(), 1, comb, knobH });
			items.push_back({ character_.get(), 1, comb, knobH });
			const auto add = [&](std::span<const P> grid) {
				for (const P p : grid) {
					items.push_back({ knob(p), 1, true, knobH });
				}
			};
			add(mode == ero::Mode::Dust ? std::span<const P>(kDustEngine)
			                            : comb ? std::span<const P>(kCombEngine) : std::span<const P>(kRustEngine));
			if (!comb) {
				add(kShakeRow);
			}
			items.push_back({ blend_.get(), 1, comb, knobH });
			items.push_back({ gamma_.get(), 2, comb, knobH });
			// TWO ROWS: as many columns as half the cells, rounded up (Gamma, two cells, counts as two).
			int cells = 0;
			for (const FlowItem& item : items) {
				cells += item.shown ? item.span : 0;
			}
			const int columns = std::max(2, (cells + 1) / 2);
			group("Engine", x0, sideW, columns, items);
			for (SwitchChip* chip : { blend_.get() }) {
				if (chip->isVisible()) {
					centred(chip, SwitchChip::kHeight);
				}
			}
			if (gamma_->isVisible()) {
				centred(gamma_.get(), ValueBox::kCompactHeight);
			}
		}
		closeRow(first, kGroupCaption + contentH + kGroupPad);
	}

	// ROW THREE: FEEDBACK (Resonance, the outer loop and its interval) beside OUTPUT (HP Wet, Follow, Mix).
	{
		const size_t first = groups_.size();
		const float boxW = (sideW - kGroupGap) * 0.5f;
		group("Feedback", x0, boxW, 3, { { knob(P::Resonance), 1, !comb, knobH }, { loopPair_.get(), 2, true, knobH } });
		group("Output", x0 + boxW + kGroupGap, boxW, 3,
		      { { knob(P::HpWet), 1, true, knobH }, { knob(P::Follow), 1, true, knobH }, { knob(P::Mix), 1, true, knobH } });
		closeRow(first);
	}

	// THE BLOCK DIAGRAM, with its four switches in it, and under it Track: one fixed place in every engine (hidden in the Comb,
	// which has no Freq to follow a note).
	const float trackH = SwitchChip::kHeight;
	const float flowY = std::max(y, static_cast<float>(height()) - kPad - trackH - kGroupGap - EroderFlow::kHeight);
	flow_->setBounds(x0, flowY, sideW, EroderFlow::kHeight);
	flow_->setVisible(true);
	(void)flow_->update();
	flow_->resized();
	track_->setBounds(x0 + kGroupPad, flowY + EroderFlow::kHeight + kGroupGap, 90.0f, trackH);
	track_->setVisible(!comb);
	refreshHonesty();
}

void EroderExpanded::refreshHonesty() {
	ero::Row row {};
	(void)readModulatedRow(dhost_, device_, ero::kDescriptor, row);
	const float source = row[static_cast<size_t>(ero::Param::Source)];
	// WIDTH IS THE NOISE BAND'S (`EroderCard::refreshHonesty`); Detune and Mod Fold are the sine's.
	const bool comb = engine() == ero::Mode::Comb;
	const bool emphasised = row[static_cast<size_t>(ero::Param::EmphAmount)] != 0.0f;
	const bool bell = row[static_cast<size_t>(ero::Param::EmphShape)] >= 0.5f;
	// THE BELL'S WIDTH IS WIDTH (the Comb has no band, so there it is the bell's alone).
	const bool etch = engine() == ero::Mode::Etch;
	// THE FILTER'S ROWS: Freq is the running engine's own (the Comb has none, so there it is not drawn); Width is the noise band's
	// in Dust and Rust, the Bell's alone in the Comb and Etch.
	filter_->setBoxShown(0, !comb);
	filter_->setBoxEnabled(1, (!comb && !etch && source > 0.0f) || (bell && emphasised));
	knob(P::Detune)->setActive(source < 1.0f);
	knob(P::ModFold)->setActive(source < 1.0f);
	// SUB THRU GIVES BACK WHAT HP WET TOOK OF A DRY THAT WAS TURNED DOWN: with no filter there is nothing to give
	// back, and in the Comb the dry is whole unless Blend crossfades it.
	const bool filtering = static_cast<double>(row[static_cast<size_t>(ero::Param::HpWet)]) > ero::kHpWetLog2Min;
	const bool crossfades = engine() != ero::Mode::Comb || row[static_cast<size_t>(ero::Param::Blend)] >= 0.5f;
	subThru_->setEnabled(filtering && crossfades);
	// THE EMPHASIS'S OWN: the shape, the offset and the restore are nothing at zero.
	emphasis_->setBoxEnabled(1, emphasised);
	emphasis_->setSelectorEnabled(emphasised);
	restore_->setEnabled(emphasised);
	// THE INTERVAL IS THE OUTER LOOP'S: at Loop 0 there is no outer delay.
	const bool outer = row[static_cast<size_t>(ero::Param::Loop)] > 0.0f;
	knob(P::LoopInterval)->setActive(outer);
	// THE INNER LOOP: the Comb's never stops; Dust's and Rust's is Resonance (either sign).
	const bool inner = comb || row[static_cast<size_t>(ero::Param::Resonance)] != 0.0f;
	tap_->setEnabled(inner);
	inLoop_->setEnabled(emphasised && (inner || outer));
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
	if (sub_->update()) {
		changed = true;
	}
	// THE PICTURE REDRAWS ITSELF (`EroderFlow::update`); its change is not the window's.
	(void)flow_->update();
	refreshHonesty();
	if (changed) {
		redraw();
	}
	return changed;
}

void EroderExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width(), height());
	// THE GROUPS' OUTLINES, each captioned over its top edge (a `PairCell`'s way).
	const visage::Font& face = font(kKnobLabelSize);
	for (const Group& g : groups_) {
		const float top = g.y + kGroupCaption - PairCell::kCaptionHeight * 0.5f - 3.0f;
		canvas.setColor(withAlpha(kLine, 0.35f));
		canvas.roundedRectangleBorder(g.x + 0.5f, top, g.w - 1.0f, g.y + g.h - top - 0.5f, PairCell::kRadius, 1.0f);
		std::string caption = g.caption;
		for (char& c : caption) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		const float textW = trackedWidth(face, caption, kKnobLabelTracking);
		canvas.setColor(kSurfaceRaised);
		canvas.fill(g.x + 8.0f, top - 1.0f, textW + 8.0f, 3.0f);
		canvas.setColor(kMuted);
		drawTracked(canvas, caption, face, kKnobLabelTracking, g.x + 12.0f, top - 6.0f, 9.0f);
	}
}

std::vector<ero::Param> EroderExpanded::addressedParamsForTest() const {
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
