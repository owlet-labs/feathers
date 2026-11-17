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
	{ ero::Param::Amount, "Amount" },   { ero::Param::Freq, "Freq" },         { ero::Param::Width, "Width" },
	{ ero::Param::Source, "Source" },   { ero::Param::Grit, "Grit" },         { ero::Param::Stereo, "Stereo" },
	{ ero::Param::Resonance, "Reso" },  { ero::Param::Follow, "Follow" },     { ero::Param::Mix, "Mix" },
	{ ero::Param::HpWet, "HP Wet" },    { ero::Param::Step, "Step" },         { ero::Param::Detune, "Detune" },
	{ ero::Param::ModFold, "Mod Fold" }, { ero::Param::RustBias, "Bias" },    { ero::Param::RustRectify, "Rectify" },
	{ ero::Param::Cutoff, "Cutoff" },   { ero::Param::Heat, "Heat" },
	{ ero::Param::EmphAmount, "Emphasis" }, { ero::Param::Smear, "Smear" }, { ero::Param::Loop, "Loop" },
	{ ero::Param::LoopInterval, "Interval" },
};

/// A FRAME THAT KEEPS ITS ONE CHILD THE CHILD'S OWN HEIGHT AND CENTRES IT: the Tilt | Bell strip is a chip tall, a pair's
/// half is a knob tall.
class CentredCell final : public visage::Frame {
public:
	explicit CentredCell(visage::Frame& child, float childHeight) : child_(child), childHeight_(childHeight) {
		addChild(&child);
		// THE CELL IS NOT A CONTROL: presses pass through to its child (the default would answer for the child).
		setIgnoresMouseEvents(true, true);
	}
	void resized() override {
		const float h = std::min(childHeight_, static_cast<float>(height()));
		const float inset = 3.0f;
		child_.setBounds(inset, (static_cast<float>(height()) - h) * 0.5f, std::max(0.0f, static_cast<float>(width()) - 2.0f * inset), h);
	}

private:
	visage::Frame& child_;
	float childHeight_;
};

using P = ero::Param;
/// THE SHAKE'S EXTRAS, after the engine's own cells: Dust's and Rust's.
constexpr P kShakeRow[] = { P::Step, P::Detune, P::ModFold };
/// THE ENGINE GROUP'S CELLS: the grids above without what the Feedback and Output groups hold.
constexpr P kDustEngine[] = { P::Amount, P::Freq, P::Width, P::Source, P::Grit, P::Stereo };
constexpr P kRustEngine[] = { P::RustBias, P::RustRectify, P::Freq, P::Width, P::Source, P::Grit, P::Stereo };
constexpr P kCombEngine[] = { P::Cutoff, P::Heat, P::Width };
constexpr P kEtchEngine[] = { P::Freq, P::Width };

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

	// EMPHASIS, A PAIR: the amount and its shape (a strip of two, both shown before one is chosen). LOOP, A PAIR: the
	// outer loop's amount and the interval it rings at.
	shape_ = std::make_unique<SegmentedControl>(control(ero::Param::EmphShape), registry, ero::kEmphShapeNames, notifier_);
	shapeCell_ = std::make_unique<CentredCell>(*shape_, SwitchChip::kHeight);
	emphasisPair_ = std::make_unique<PairCell>("Emphasis", *knob(ero::Param::EmphAmount), *shapeCell_);
	addChild(emphasisPair_.get());
	loopPair_ = std::make_unique<PairCell>("Loop", *knob(ero::Param::Loop), *knob(ero::Param::LoopInterval));
	addChild(loopPair_.get());

	// DRIVE, A PAIR (`EroderCard`'s constructor).
	// ETCH'S SLOT ROW, captioned (the window has the room).
	etch_.build([this](ero::Param p) -> DeviceControl& { return control(p); }, kFace, notifier_, "Slots");
	addChild(etch_.outer.get());

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
	etch_.setModulation(mod, history);
}

std::string EroderExpanded::title() const { return ero::kTraits.name; }

visage::Point EroderExpanded::minimumSize() const {
	// THE TALLEST ENGINE'S COLUMN, group by group: the Dust's and Rust's three rows of cells, or Etch's slot row and a row.
	const float knobH = Knob::columnHeight(kFace);
	const float pairH = PairCell::heightFor(knobH);
	const float rows3 = 3.0f * knobH + 2.0f * kRowGap;
	const float etchH = etch_.heightFor(knobH) + kRowGap + knobH;
	const float groups = groupHeight(pairH) + groupHeight(std::max(rows3, etchH)) + groupHeight(pairH) + groupHeight(pairH)
	                     + groupHeight(knobH) + 4.0f * kGroupGap;
	const float side = kSwitchHeight + kGroupGap + groups + kGroupGap + EroderFlow::kHeight + 2.0f * kPad;
	return { 792.0f, std::max(380.0f, side) };
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
	const float sideW = kSideWidth + 2.0f * kGroupPad;
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - sideW - kGap);
	plot_->setBounds(kPad, kPad, plotW, std::max(0.0f, height() - 2.0f * kPad));

	const float x0 = width() - kPad - sideW;
	const float x = x0 + kGroupPad;
	float y = kPad;
	mode_->setBounds(x0, y, sideW, kSwitchHeight);
	y += kSwitchHeight + kGroupGap;

	const float cell = kSideWidth / static_cast<float>(kPerRow);
	const float knobH = Knob::columnHeight(kFace);
	const float pairH = PairCell::heightFor(knobH);
	groups_.clear();
	// EVERYTHING IS HIDDEN, THEN THE ENGINE'S OWN ARE FLOWED: a hidden item takes no room, and a pair wraps whole
	// (`flowCells`).
	for (const auto& entry : knobs_) {
		// THE PAIRS' KNOBS ARE SHOWN BY THEIR PAIR.
		const P p = entry.first;
		if (p != P::EmphAmount && p != P::Loop && p != P::LoopInterval) {
			entry.second->setVisible(false);
		}
	}
	etch_.setShown(etch);
	// A GROUP: its caption band, its items flowed four across, and its outline round them.
	const auto group = [&](const char* caption, const std::vector<FlowItem>& items) {
		const float top = y;
		const float under = flowCells(items, static_cast<int>(kPerRow), x, y + kGroupCaption, cell, kRowGap);
		const float bottom = under + kGroupPad;
		groups_.push_back({ caption, x0, top, sideW, bottom - top });
		y = bottom + kGroupGap;
	};
	// A KNOB BESIDE A PAIR sits level with the cells inside it; a chip is centred in its cell's height.
	const auto level = [&](visage::Frame* f, const visage::Frame& pair) {
		f->setBounds(f->x(), pair.y() + PairCell::kCaptionHeight, f->width(), knobH);
	};
	const auto centre = [&](SwitchChip& chip, float top) {
		chip.setBounds(chip.x() + 3.0f, top + (knobH - SwitchChip::kHeight) * 0.5f, cell - 6.0f, SwitchChip::kHeight);
	};

	// TONE: the emphasis (amount and shape), then Smear.
	group("Tone", { { emphasisPair_.get(), 2, true, pairH }, { knob(P::Smear), 1, true, knobH } });
	level(knob(P::Smear), *emphasisPair_);

	// ENGINE: Etch's slots lead its group; the Comb's coordinates lead its; then the engine's own cells.
	{
		std::vector<FlowItem> items;
		items.push_back({ etch_.outer.get(), static_cast<int>(kPerRow), etch, etch_.heightFor(knobH) });
		items.push_back({ tone_.get(), 1, comb, knobH });
		items.push_back({ character_.get(), 1, comb, knobH });
		const auto add = [&](std::span<const P> grid) {
			for (const P p : grid) {
				items.push_back({ knob(p), 1, true, knobH });
			}
		};
		add(mode == ero::Mode::Dust ? std::span<const P>(kDustEngine)
		                            : comb ? std::span<const P>(kCombEngine)
                                           : etch ? std::span<const P>(kEtchEngine) : std::span<const P>(kRustEngine));
		if (!comb && !etch) {
			add(kShakeRow);
		}
		items.push_back({ track_.get(), 1, !comb, knobH });
		items.push_back({ blend_.get(), 1, comb, knobH });
		items.push_back({ gamma_.get(), 2, comb, knobH });
		group("Engine", items);
		// THE CHIPS AND GAMMA ARE CENTRED IN THEIR CELLS' HEIGHT.
		for (SwitchChip* chip : { track_.get(), blend_.get() }) {
			if (chip->isVisible()) {
				centre(*chip, chip->y());
			}
		}
		if (gamma_->isVisible()) {
			gamma_->setBounds(gamma_->x() + 3.0f, gamma_->y() + (knobH - ValueBox::kCompactHeight) * 0.5f,
			                  2.0f * cell - 6.0f, ValueBox::kCompactHeight);
		}
	}

	// DRIVE: the amount and what it pushes into.
	group("Drive", { { drivePair_.get(), 2, true, pairH } });

	// FEEDBACK: Resonance (Dust, Rust, Etch), the outer loop and its interval.
	group("Feedback", { { knob(P::Resonance), 1, !comb, knobH }, { loopPair_.get(), 2, true, pairH } });
	level(knob(P::Resonance), *loopPair_);

	// OUTPUT: HP Wet, Follow, Mix. (Sub Thru, which gives back what HP Wet takes, is seated in the picture.)
	group("Output", { { knob(P::HpWet), 1, true, knobH }, { knob(P::Follow), 1, true, knobH }, { knob(P::Mix), 1, true, knobH } });

	// THE TOPOLOGY PICTURE, at the foot of the column, with its three switches in it.
	flow_->setBounds(x0, std::max(y, static_cast<float>(height()) - kPad - EroderFlow::kHeight), sideW, EroderFlow::kHeight);
	flow_->setVisible(true);
	(void)flow_->update();
	flow_->resized();
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
	knob(P::Width)->setActive((!comb && !etch && source > 0.0f) || (bell && emphasised));
	knob(P::Detune)->setActive(source < 1.0f);
	knob(P::ModFold)->setActive(source < 1.0f);
	// SUB THRU GIVES BACK WHAT HP WET TOOK OF A DRY THAT WAS TURNED DOWN: with no filter there is nothing to give
	// back, and in the Comb the dry is whole unless Blend crossfades it.
	const bool filtering = static_cast<double>(row[static_cast<size_t>(ero::Param::HpWet)]) > ero::kHpWetLog2Min;
	const bool crossfades = engine() != ero::Mode::Comb || row[static_cast<size_t>(ero::Param::Blend)] >= 0.5f;
	subThru_->setEnabled(filtering && crossfades);
	// THE EMPHASIS'S OWN: the shape and the restore are nothing at zero.
	shape_->setActive(emphasised);
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
