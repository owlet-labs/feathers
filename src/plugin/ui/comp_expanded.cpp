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

#include "plugin/ui/comp_expanded.h"

#include "fx/comp/comp_lanes.h"
#include "plugin/ui/control_face.h"
#include "plugin/ui/theme.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace feathers::ui {

namespace cp = fx::comp;

namespace {

rack::InnerChain* laneOf(void* surface) {
	return cp::companderLaneOf(surface);
}

/// THE KNOBS, IN FLOW ORDER — the card's order, then the second half's three (backlog QG), which close the grid.
constexpr std::array<cp::Param, 15> kKnobs {
	cp::Param::Drive,   cp::Param::Threshold, cp::Param::Ratio,      cp::Param::CmpRatio,  cp::Param::Knee,
	cp::Param::CmpRange, cp::Param::Attack,   cp::Param::Ceiling,    cp::Param::Release,   cp::Param::Makeup,
	cp::Param::Mix,     cp::Param::StereoLink, cp::Param::CmpRatio2, cp::Param::CmpAttack2, cp::Param::CmpRelease2,
};
constexpr std::array<const char*, 15> kCaptions {
	"Drive", "Thresh", "Ratio", "Ratio", "Knee", "Range", "Attack", "Ceiling", "Release", "Makeup", "Mix", "Link",
	"Ratio 2", "Attack 2", "Release 2",
};

} // namespace

//------------------------------------------------------------------------------------------------//
// THE PLOT                                                                                        //
//------------------------------------------------------------------------------------------------//

comp_plot::Curves CompTransferView::curves() const {
	return comp_plot::curvesOf([this](size_t i) { return host_.fieldAt(id_, i); });
}

void CompTransferView::draw(visage::Canvas& canvas) {
	const bool has = host_.readingCountAt(id_) >= static_cast<size_t>(cp::Reading::kCount);
	const float in = has ? host_.readingAt(id_, static_cast<size_t>(cp::Reading::InPeak)) : 0.0f;
	comp_plot::paint(canvas, visage::Bounds { 0.0f, 0.0f, width(), height() }, curves(), has, in);
}

//------------------------------------------------------------------------------------------------//
// THE WINDOW                                                                                      //
//------------------------------------------------------------------------------------------------//

comp_plot::Meter CompMeterView::meter() const {
	return comp_plot::meterOf(host_.readingCountAt(id_), [this](size_t i) { return host_.readingAt(id_, i); });
}

void CompMeterView::draw(visage::Canvas& canvas) {
	comp_plot::paintMeter(canvas, visage::Bounds { 0.0f, 0.0f, width(), height() }, meter());
}

CompExpanded::CompExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), id_(id), notifier_(notifier), chainPin_(host) {
	controls_.resize(static_cast<size_t>(cp::Param::kCount));
	plot_ = std::make_unique<CompTransferView>(host, id);
	addChild(plot_.get());
	meter_ = std::make_unique<CompMeterView>(host, id);
	addChild(meter_.get());
	mode_ = std::make_unique<SegmentedControl>(control(cp::Param::Mode), registry, cp::kModeNames, notifier_);
	addChild(mode_.get());
	topology_ = std::make_unique<SegmentedControl>(control(cp::Param::Topology), registry, cp::kTopologyNames, notifier_);
	addChild(topology_.get());
	link_ = std::make_unique<SwitchChip>(control(cp::Param::CmpLink), "Link");
	// THE UNLINK COPY HAPPENS BEFORE THE TOGGLE, so the block that first runs unlinked runs at the first half's values.
	link_->setPressOverride([this](const visage::MouseEvent&) {
		if (control(cp::Param::CmpLink).value() >= 0.5) {
			unlink();
		}
		return false; // and the chip toggles as it would have
	});
	addChild(link_.get());
	stereo_ = std::make_unique<SwitchChip>(control(cp::Param::StereoMode), "Stereo");
	addChild(stereo_.get());
	detect_ = std::make_unique<SegmentedControl>(control(cp::Param::Detect), registry, cp::kDetectNames, notifier_);
	addChild(detect_.get());
	detector_ = std::make_unique<SegmentedControl>(control(cp::Param::Detector), registry, cp::kDetectorNames, notifier_);
	addChild(detector_.get());
	scHpf_ = std::make_unique<ValueBox>(control(cp::Param::ScHpf), "SC HPF");
	scHpf_->setCompact(true);
	addChild(scHpf_.get());
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), kCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// THE LANE'S RAIL — over the lane when there is one, a phantom while there is not (`followLane`). The Delay's.
	lane_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kLaneLabel, kLaneLine, &laneOf }, registry, notifier_);
	addChild(lane_.get());
	lane_->onRailWidthChanged([this]() {
		resized();
		redraw();
		minimumSizeChanged();
	});
	void* const held = host.containerSurfaceOf(id);
	(void)followLane(held);
	chainPin_.hold(held);
	(void)lane_->setCollapsed(false);
	(void)applyGates();
}

CompExpanded::~CompExpanded() = default;

DeviceControl& CompExpanded::control(cp::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, id_, cp::kDescriptor, i);
	}
	return *controls_[i];
}

bool CompExpanded::live(cp::Param p) const {
	return cp::liveIn(p, [this](size_t i) { return host_.fieldAt(id_, i); });
}

bool CompExpanded::compander() const {
	return std::lround(host_.fieldAt(id_, static_cast<size_t>(cp::Param::Mode))) == static_cast<long>(cp::Mode::Compander);
}

Knob* CompExpanded::knobForTest(cp::Param p) const {
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == p) {
			return knobs_[i].get();
		}
	}
	return nullptr;
}

void CompExpanded::unlink() {
	const auto copy = [this](cp::Param from, cp::Param to) {
		DeviceControl& c = control(to);
		c.beginGesture();
		c.setValue(control(from).value());
		c.endGesture();
	};
	copy(cp::Param::CmpRatio, cp::Param::CmpRatio2);
	copy(cp::Param::Attack, cp::Param::CmpAttack2);
	copy(cp::Param::Release, cp::Param::CmpRelease2);
}

void CompExpanded::pressLinkForTest() {
	if (control(cp::Param::CmpLink).value() >= 0.5) {
		unlink();
	}
	link_->toggleForTest();
}

bool CompExpanded::followLane(void* held) {
	bool moved = lane_->follow(held);
	// A PHANTOM EXACTLY WHILE THERE IS NO LANE: its (+) builds one and opens its add menu once its column exists.
	const bool wantPhantom = laneOf(held) == nullptr;
	if (wantPhantom != phantomShown_) {
		phantomShown_ = wantPhantom;
		if (wantPhantom) {
			lane_->setPhantom(kLaneEmpty, [this] {
				if (cp::ensureCompanderLane(host_.containerSurfaceOf(id_)) != nullptr) {
					pendingAddMenu_ = true;
				}
			});
		}
		else {
			lane_->setPhantom({}, {});
		}
		moved = true;
	}
	return moved;
}

float CompExpanded::railWidth() const {
	// THE RAIL IS THE COMPANDER'S: in Compress and Limit it is not drawn and takes no room (its devices are kept).
	return compander() ? lane_->railWidth() : 0.0f;
}

visage::Point CompExpanded::minimumSize() const {
	const float face = kPlotWidth + kGap + static_cast<float>(kKnobsPerRow) * kCellWidth + kGap + kMeterWidth;
	const float rows = 2.0f * (kSwitchHeight + kRowGap) + static_cast<float>(kKnobRows) * (kCellHeight + kRowGap)
	                   + kSwitchHeight + kRowGap;
	visage::Point p { 2.0f * kPad + face + railWidth(), 2.0f * kPad + rows };
	if (compander() && !lane_->collapsed()) {
		p.y = std::max(p.y, lane_->openMinimumHeight());
	}
	return p;
}

void CompExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (auto& k : knobs_) {
		k->setModulation(mod, history);
	}
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<cp::Param>(i)).setModulation(mod, history);
	}
	lane_->setOuterModulation(mod, history, [this] { return watchedDevice(); });
}

void CompExpanded::setPresentedOn(SurfaceLayers* host) { lane_->setPresentedOn(host); }

void CompExpanded::onInnerNestChanged() { lane_->setNest(innerNest()); }

void CompExpanded::resized() { layOut(); }

void CompExpanded::layOut() {
	laidOutMode_ = static_cast<int>(std::lround(host_.fieldAt(id_, static_cast<size_t>(cp::Param::Mode))));
	const float rail = railWidth();
	lane_->setBounds(width() - rail, 0.0f, rail, height());
	lane_->setVisible(rail > 0.0f);
	const float left = kPad;
	const float colW = std::max(0.0f, width() - rail - 2.0f * kPad);
	// EVERY `setBounds` UNCONDITIONAL (CLAUDE.md's DPI rule).
	float y = kPad;
	mode_->setBounds(left, y, std::max(0.0f, colW - kChipWidth - kGap), kSwitchHeight);
	stereo_->setBounds(left + colW - kChipWidth, y + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kChipWidth,
	                   SwitchChip::kHeight);
	y += kSwitchHeight + kRowGap;
	topology_->setBounds(left, y, std::max(0.0f, colW - kChipWidth - kGap), kSwitchHeight);
	link_->setBounds(left + colW - kChipWidth, y + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kChipWidth,
	                 SwitchChip::kHeight);
	y += kSwitchHeight + kRowGap;
	const float gridH = static_cast<float>(kKnobRows) * (kCellHeight + kRowGap) - kRowGap;
	plot_->setBounds(left, y, kPlotWidth, gridH);
	// THE LIVE KNOBS, CLOSED UP, as on the card.
	const float gridX = left + kPlotWidth + kGap;
	// THE METER AT THE RIGHT, the grid between it and the plot.
	meter_->setBounds(left + colW - kMeterWidth, y, kMeterWidth, gridH);
	const float cell = std::max(0.0f, (colW - kPlotWidth - kGap - kMeterWidth - kGap) / static_cast<float>(kKnobsPerRow));
	size_t at = 0;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const bool on = live(kKnobs[i]);
		knobs_[i]->setBounds(gridX + cell * static_cast<float>(at % kKnobsPerRow),
		                     y + static_cast<float>(at / kKnobsPerRow) * (kCellHeight + kRowGap), cell, kCellHeight);
		if (on) {
			++at;
		}
		if (kKnobs[i] == cp::Param::Threshold) {
			knobs_[i]->setLabel(compander() ? "Ref" : kCaptions[i]);
		}
	}
	y += gridH + kRowGap;
	const float third = (colW - 2.0f * kGap) / 3.0f;
	detect_->setBounds(left, y, third, kSwitchHeight);
	scHpf_->setBounds(left + third + kGap, y + (kSwitchHeight - ValueBox::kCompactHeight) * 0.5f, third,
	                  ValueBox::kCompactHeight);
	detector_->setBounds(left + 2.0f * (third + kGap), y, third, kSwitchHeight);
	(void)applyGates();
}

bool CompExpanded::applyGates() {
	bool changed = false;
	const auto show = [&](visage::Frame& f, bool on) {
		if (f.isVisible() != on) {
			f.setVisible(on);
			changed = true;
		}
	};
	for (size_t i = 0; i < knobs_.size(); ++i) {
		show(*knobs_[i], live(kKnobs[i]));
	}
	show(*topology_, live(cp::Param::Topology));
	show(*link_, live(cp::Param::CmpLink));
	show(*detect_, live(cp::Param::Detect));
	show(*detector_, live(cp::Param::Detector));
	return changed;
}

bool CompExpanded::updateContent() {
	bool moved = false;
	const int mode = static_cast<int>(std::lround(host_.fieldAt(id_, static_cast<size_t>(cp::Param::Mode))));
	// A GATE THAT MOVED RE-FLOWS THE GRID (the closed-up cells), as a Mode change does on the card.
	std::array<bool, 15> was {};
	for (size_t i = 0; i < knobs_.size(); ++i) {
		was[i] = knobs_[i]->isVisible();
	}
	if (applyGates()) {
		moved = true;
	}
	bool reflow = mode != laidOutMode_;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		reflow = reflow || was[i] != knobs_[i]->isVisible();
	}
	if (reflow) {
		const bool sizeMoves = (mode == static_cast<int>(cp::Mode::Compander)) != (laidOutMode_ == static_cast<int>(cp::Mode::Compander));
		layOut();
		if (sizeMoves) {
			minimumSizeChanged();
		}
		moved = true;
	}
	// THE PLOT FOLLOWS EVERY FIELD THAT SHAPES IT, and the input peak its dot sits at.
	std::array<float, 16> curve {};
	{
		const auto c = plot_->curves().s;
		curve = { static_cast<float>(c.mode), c.thresholdDb, c.ratio,      c.kneeDb,   c.cmpRatio,  c.cmpRangeDb,
			      static_cast<float>(c.topology), c.cmpLink ? 1.0f : 0.0f, c.cmpRatio2, c.driveDb, c.makeupDb, c.mix,
			      c.ceilingDb, 0.0f, 0.0f, 0.0f };
		const bool has = host_.readingCountAt(id_) >= static_cast<size_t>(cp::Reading::kCount);
		curve[13] = has ? 1.0f : 0.0f;
		curve[14] = has ? host_.readingAt(id_, static_cast<size_t>(cp::Reading::InPeak)) : 0.0f;
	}
	// THE METER, compared rather than assumed (`CompCard::updateContent`): published-to-unpublished is a change too.
	if (const comp_plot::Meter m = meter_->meter(); !(m == drawnMeter_)) {
		drawnMeter_ = m;
		meter_->redraw();
		moved = true;
	}
	if (curve != drawnCurve_) {
		drawnCurve_ = curve;
		plot_->redraw();
		moved = true;
	}
	if (moved) {
		redraw();
	}
	// THE LANE, RE-ASKED EVERY TICK: the handle is contemporaneous (trap T9).
	void* const held = host_.containerSurfaceOf(id_);
	moved = followLane(held) || moved;
	moved = lane_->updateCards() > 0 || moved;
	chainPin_.hold(held);
	if (pendingAddMenu_ && lane_->column() != nullptr) {
		pendingAddMenu_ = false;
		lane_->column()->showAddMenu();
		moved = true;
	}
	return moved;
}

void CompExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width() - railWidth(), height());
}

std::vector<cp::Param> CompExpanded::addressedParamsForTest() const {
	std::vector<cp::Param> out;
	std::vector<ControlFace::Offered> offered;
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ControlFace*>(frame)) {
			offered.clear();
			face->offeredControls(offered);
			for (const ControlFace::Offered& o : offered) {
				for (size_t i = 0; i < controls_.size(); ++i) {
					const auto p = static_cast<cp::Param>(i);
					if (o.road == ControlFace::Road::Face && controls_[i] != nullptr
					    && static_cast<const device::Modulatable*>(controls_[i].get()) == o.control
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
