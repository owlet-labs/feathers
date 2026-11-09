/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/twinfield_card.h"

#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <string>

namespace feathers::ui {

namespace {

namespace tf = feathers::mod::twinfield;
using LP = tf::LayerParam;

constexpr const char* const kTabNames[] = { "A", "B" };
static_assert(std::size(kTabNames) == tf::kLayers);

constexpr device::ParamDesc kViewDesc { .key = "view", .name = "Layer", .min = 0.0, .max = 1.0, .def = 0.0,
	                                    .stepped = true, .unit = device::Unit::Choice, .names = kTabNames,
	                                    .modulatable = false,
	                                    .tooltip = "Which layer's terrain the pad shows and the rows below edit" };

constexpr LP kViewRow[] = { LP::PanX, LP::PanY, LP::Zoom, LP::Rotate, LP::Gamma };
constexpr const char* kViewLabels[] = { "Pan X", "Pan Y", "Zoom", "Rotate", "Gamma" };

} // namespace

const device::ParamDesc& TwinFieldCard::ViewChoice::desc() const {
	return kViewDesc;
}

void TwinFieldCard::ViewChoice::setValue(double value) {
	card_.setShown(static_cast<size_t>(std::lround(std::clamp(value, 0.0, 1.0))));
}

std::string TwinFieldCard::ViewChoice::text() const {
	return kTabNames[card_.shown_];
}

TwinFieldCard::TwinFieldCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                             Notifier* notifier, ModState* mod, EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	drawn_.fill(std::numeric_limits<double>::quiet_NaN());
	tabs_ = std::make_unique<SegmentedControl>(view_, registry, kTabNames, notifier_);
	addChild(tabs_.get());

	std::array<TwinFieldPad::LayerControls, tf::kLayers> padControls {};
	for (size_t l = 0; l < tf::kLayers; ++l) {
		for (size_t p = 0; p < tf::kLayerParams; ++p) {
			padControls[l][p] = &control(tf::layerParam(l, static_cast<LP>(p)));
		}
	}
	pad_ = std::make_unique<TwinFieldPad>(padControls);
	// THE ORBIT'S PHASES, from the channel the output cells already read (`liveOutputs`).
	pad_->setPhaseReader([this](size_t layer, double* phase) { return publishedPhase(layer, phase); });
	addChild(pad_.get());

	// THE SOLO CHIPS SIT ON THEIR TABS, added after the tabs so they take the press.
	for (size_t l = 0; l < tf::kLayers; ++l) {
		solo_[l] = std::make_unique<SwitchChip>(control(tf::at(l == 0 ? tf::Param::SoloA : tf::Param::SoloB)),
		                                        "Solo");
		addChild(solo_[l].get());
	}

	// THE CROSSFADE, ON THE PLOT'S CORNER — it is about both layers, so it is not in either tab's rows.
	crossfade_ = std::make_unique<Knob>(control(tf::at(tf::Param::Crossfade)), "X-Fade", notifier_);
	addChild(crossfade_.get());

	for (size_t l = 0; l < tf::kLayers; ++l) {
		LayerWidgets& w = layers_[l];
		for (size_t i = 0; i < std::size(kViewRow); ++i) {
			w.view[i] = std::make_unique<Knob>(control(tf::layerParam(l, kViewRow[i])), kViewLabels[i], notifier_);
			addChild(w.view[i].get());
		}
		w.rate = std::make_unique<Knob>(control(tf::layerParam(l, LP::Rate)), "Rate", notifier_);
		w.sync = std::make_unique<ValueBox>(control(tf::layerParam(l, LP::Sync)), "Sync");
		w.mode = std::make_unique<ValueBox>(control(tf::layerParam(l, LP::Mode)), "Mode");
		w.depth = std::make_unique<Knob>(control(tf::layerParam(l, LP::Depth)), "Depth", notifier_);
		w.spread = std::make_unique<Knob>(control(tf::layerParam(l, LP::Spread)), "Spread", notifier_);
		for (ValueBox* box : { w.sync.get(), w.mode.get() }) {
			box->onPicker([this, box] {
				showMenuAt(guardedMenuFor(*box), *box, { 0.0f, 0.0f, box->width(), box->height() });
			});
		}
		addChild(w.rate.get());
		addChild(w.sync.get());
		addChild(w.mode.get());
		addChild(w.depth.get());
		addChild(w.spread.get());
	}
	for (Knob* knob : allKnobs()) {
		knob->setFaceDiameter(kKnobDiameter);
		knob->setModulation(mod_, history_);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	showLayerWidgets();
}

TwinFieldCard::~TwinFieldCard() {
	alive_.reset();
}

std::vector<Knob*> TwinFieldCard::allKnobs() const {
	std::vector<Knob*> out { crossfade_.get() };
	for (const LayerWidgets& w : layers_) {
		for (const auto& k : w.view) {
			out.push_back(k.get());
		}
		out.push_back(w.rate.get());
		out.push_back(w.depth.get());
		out.push_back(w.spread.get());
	}
	return out;
}

DeviceControl& TwinFieldCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, tf::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

visage::PopupMenu TwinFieldCard::guardedMenuFor(ValueBox& box) {
	visage::PopupMenu menu = box.choiceMenu();
	const std::weak_ptr<void> alive = alive_;
	const visage::CallbackList<void(int)> pick = menu.onSelection();
	menu.onSelection() = [alive, pick](int id) {
		if (!alive.expired()) {
			pick.callback(id);
		}
	};
	return menu;
}

device::Modulatable* TwinFieldCard::enableModulatable() {
	return &control(tf::at(tf::Param::Enabled));
}

void TwinFieldCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* knob : allKnobs()) {
		knob->setModulation(mod, history);
	}
}

void TwinFieldCard::show(size_t layer) {
	tabs_->select(static_cast<int>(std::min(layer, tf::kLayers - 1)));
}

void TwinFieldCard::setShown(size_t layer) {
	layer = std::min(layer, tf::kLayers - 1);
	if (layer == shown_) {
		return;
	}
	shown_ = layer;
	pad_->setLayer(layer);
	showLayerWidgets();
	redraw();
}

void TwinFieldCard::showLayerWidgets() {
	const bool open = !folded();
	for (size_t l = 0; l < tf::kLayers; ++l) {
		const bool on = open && l == shown_;
		LayerWidgets& w = layers_[l];
		for (auto& k : w.view) {
			k->setVisible(on);
		}
		w.rate->setVisible(on);
		w.sync->setVisible(on);
		w.mode->setVisible(on);
		w.depth->setVisible(on);
		w.spread->setVisible(on);
	}
}

std::vector<tf::Param> TwinFieldCard::addressedParamsForTest() const {
	std::vector<tf::Param> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(static_cast<tf::Param>(road->control()));
		}
	};
	for (const auto& s : solo_) {
		add(&s->control());
	}
	for (Knob* knob : allKnobs()) {
		add(knob->control());
	}
	for (const LayerWidgets& w : layers_) {
		add(&w.sync->control());
		add(&w.mode->control());
	}
	return out;
}

float TwinFieldCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float TwinFieldCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

void TwinFieldCard::resized() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	float y = bodyTopEdge() + kGap;

	// THE TABS, each half carrying its Solo chip at its right end.
	tabs_->setBounds(left, y, inner, kTabsHeight);
	const float half = inner * 0.5f;
	const float chipY = y + (kTabsHeight - SwitchChip::kHeight) * 0.5f;
	for (size_t l = 0; l < tf::kLayers; ++l) {
		solo_[l]->setBounds(left + half * static_cast<float>(l + 1) - kSoloWidth - 2.0f, chipY, kSoloWidth,
		                    SwitchChip::kHeight);
	}
	y += kTabsHeight + kGap;

	pad_->setBounds(left, y, inner, kPadHeight);
	const float xfWidth = 44.0f;
	crossfade_->setBounds(left + inner - xfWidth - 2.0f, y + 2.0f, xfWidth, rowHeight());
	y += kPadHeight + kGap;

	const float fifth = std::max(0.0f, (inner - 4.0f * kGap) / 5.0f);
	const float cell = std::max(0.0f, (inner - 4.0f * kGap) / 5.0f); // Rate, Sync, Mode, Depth, Spread
	for (LayerWidgets& w : layers_) {
		for (size_t i = 0; i < w.view.size(); ++i) {
			w.view[i]->setBounds(left + static_cast<float>(i) * (fifth + kGap), y, fifth, rowHeight());
		}
		const float y2 = y + rowHeight() + kGap;
		w.rate->setBounds(left, y2, cell, rowHeight());
		w.sync->setBounds(left + (cell + kGap), y2, cell, ValueBox::kHeight);
		w.mode->setBounds(left + 2.0f * (cell + kGap), y2, cell, ValueBox::kHeight);
		w.depth->setBounds(left + 3.0f * (cell + kGap), y2, cell, rowHeight());
		w.spread->setBounds(left + 4.0f * (cell + kGap), y2, cell, rowHeight());
	}
}

void TwinFieldCard::foldedChanged() {
	const bool open = !folded();
	tabs_->setVisible(open);
	pad_->setVisible(open);
	crossfade_->setVisible(open);
	for (auto& s : solo_) {
		s->setVisible(open);
	}
	showLayerWidgets();
}

bool TwinFieldCard::updateContent() {
	// ANY PARAM THAT MOVED REDRAWS THE CARD'S CHILDREN; the pad re-reads its own key and rebuilds only if
	// a terrain input moved.
	bool told = false;
	for (size_t i = 0; i < drawn_.size(); ++i) {
		const double now = control(i).value();
		if (now != drawn_[i]) {
			drawn_[i] = now;
			told = true;
		}
	}
	if (told) {
		pad_->redraw();
		tabs_->redraw();
		for (auto& s : solo_) {
			s->redraw();
		}
		for (Knob* knob : allKnobs()) {
			knob->redraw();
		}
		for (LayerWidgets& w : layers_) {
			w.sync->redraw();
			w.mode->redraw();
		}
	}
	// THE ORBIT IS LIVE TOO: the pad repaints when the shown layer's published phase moves, and ONLY then, so a
	// stopped orbit costs nothing (no rebuild either: the terrain is keyed on the view, never on the phase).
	double phase = 0.0;
	const bool havePhase = publishedPhase(shown_, &phase);
	if (havePhase != haveDrawnPhase_ || (havePhase && phase != drawnPhase_)) {
		haveDrawnPhase_ = havePhase;
		drawnPhase_ = phase;
		pad_->redraw();
		told = true;
	}
	// THE 2x4 BLOCK IS LIVE: it repaints whenever the outputs it shows have moved.
	std::array<float, tf::kPoints> now {};
	const bool have = liveOutputs(now);
	if (have != haveOutputs_ || now != shownOutputs_) {
		haveOutputs_ = have;
		shownOutputs_ = now;
		redraw();
		told = true;
	}
	return told;
}

bool TwinFieldCard::publishedPhase(size_t layer, double* phase) const {
	uint32_t ordinal = 0;
	float a = 0.0f;
	if (!host_.playheadAt(device_, ordinal, a)) {
		return false;
	}
	if (layer == 0) {
		*phase = static_cast<double>(a);
		return true;
	}
	float b = 0.0f;
	if (!host_.playheadLevelAt(device_, b)) {
		return false;
	}
	*phase = static_cast<double>(b);
	return true;
}

bool TwinFieldCard::liveOutputs(std::array<float, tf::kPoints>& out) const {
	out.fill(0.0f);
	uint32_t ordinal = 0;
	float phaseA = 0.0f;
	float phaseB = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phaseA) || !host_.playheadLevelAt(device_, phaseB)) {
		return false;
	}
	std::array<float, tf::at(tf::Param::kCount)> row {};
	const size_t present = host_.fieldsPresentAt(device_);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = i < present ? host_.modulatedFieldAt(device_, i) : static_cast<float>(tf::kParamDescs[i].def);
	}
	const tf::Settings s = tf::settingsOf(row.data(), row.size());
	if (!s.enabled) {
		return true; // a bypassed device publishes rest, and the block says so
	}
	std::array<std::array<float, tf::kPoints>, tf::kLayers> readings {};
	const std::array<double, tf::kLayers> phases { phaseA, phaseB };
	for (size_t l = 0; l < tf::kLayers; ++l) {
		const tf::LayerSettings& ls = s.layers[l];
		readings[l] = tf::readLayer(tf::planeFor(ls.rotate, ls.gamma), ls, phases[l]);
	}
	for (size_t k = 0; k < tf::kPoints; ++k) {
		out[k] = tf::blend(readings[0][k], readings[1][k], s.crossfade, s.soloA, s.soloB);
	}
	return true;
}

void TwinFieldCard::drawHeadExtras(visage::Canvas& canvas) {
	drawSlotMenu(canvas, kMuted);
	if (folded() || !haveOutputs_) {
		return;
	}
	// EACH CELL OF THE 2x4 BLOCK CARRIES ITS OUTPUT'S VALUE: a level fill behind the host's arm (which draws a border,
	// an arrow and the number, and no fill), and the value itself at the cell's right where the cell has room.
	const std::vector<visage::Bounds> cells = outputChipBoundsFor(tf::kDescriptor, ownFrame().outputs, outputLabels());
	const visage::Font& readout = font(kKnobReadoutSize);
	for (size_t k = 0; k < cells.size() && k < tf::kPoints; ++k) {
		const visage::Bounds& c = cells[k];
		if (c.width() <= 0.0f) {
			continue;
		}
		const float v = std::clamp(shownOutputs_[k], 0.0f, 1.0f);
		canvas.setColor(withAlpha(kModulation, 0.30f));
		canvas.rectangle(c.x() + 1.0f, c.y() + 2.0f, (c.width() - 2.0f) * v, c.height() - 4.0f);
		// AND A SOLID RULE ALONG THE FOOT, so the level reads at a glance where the fill is faint over a lit chip.
		canvas.setColor(kModulation);
		canvas.rectangle(c.x() + 1.0f, c.bottom() - 3.0f, (c.width() - 2.0f) * v, 2.0f);
		constexpr float kValueWidth = 24.0f;
		if (c.width() >= SourceArm::widthFor("8") + kValueWidth) {
			char text[8];
			std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(v));
			canvas.setColor(kInk);
			canvas.text(text, readout, visage::Font::kRight, c.right() - kValueWidth - 4.0f, c.y(), kValueWidth,
			            c.height());
		}
	}
}

void TwinFieldCard::visibilityChanged() {
	FxCard::visibilityChanged();
	if (!isVisible() && pad_ != nullptr) {
		pad_->blur();
	}
}

bool TwinFieldCard::headSourceFocused() const {
	return mod_ != nullptr && mod_->source() == backplane::Source::output(device_, 0);
}

bool TwinFieldCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
