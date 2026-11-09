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

#include "plugin/ui/arp_card.h"

#include "mod/arp/arp.h"
#include "mod/arp/arp_core.h"
#include "mod/arp/humanize.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace feathers::ui {

namespace {

namespace ap = feathers::mod::arp;

} // namespace

ArpCard::ArpCard(backplane::DeviceHost& host, WidgetRegistry&, DeviceInstanceId id, Notifier* notifier, ModState* mod,
                 EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	controls_.resize(ap::kDescriptor.params.size());
	// THE TWO LISTS ARE VALUE BOXES, so each opens as a menu showing every choice before one is taken (CLAUDE.md: no
	// multivalue toggles); Latch is two states, so a chip.
	order_ = std::make_unique<ValueBox>(control(at(ap::Param::Order)), "Order");
	timeBase_ = std::make_unique<ValueBox>(control(at(ap::Param::TimeBase)), "Time Base");
	for (ValueBox* box : { order_.get(), timeBase_.get() }) {
		addChild(box);
	}
	// THE FEEL, WHERE SWING WAS: SKLITTER's groove cell — the names on a ZoneSelect (every feel on its list before one is
	// taken), the Amount the box over them, shown only where the feel has one (the honesty rule).
	{
		const size_t mode = at(ap::Param::Feel);
		std::vector<ZoneSelect::ZoneName> names;
		for (const char* n : ap::kParamDescs[mode].names) {
			names.push_back({ n == nullptr ? "" : n, {} });
		}
		feel_ = std::make_unique<ModeValueCell>(control(mode), "Feel", std::move(names), [this](size_t param) {
			if (param >= ap::kDescriptor.params.size()) {
				return false;
			}
			const device::ParamDesc& d = ap::kParamDescs[param];
			if (d.gateControl < 0) {
				return true;
			}
			const auto g = static_cast<size_t>(d.gateControl);
			const float v = g < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, g) : static_cast<float>(ap::kParamDescs[g].def);
			return device::relevantFor(d, v);
		});
		feel_->addFollower(at(ap::Param::FeelAmount), std::make_unique<ValueBox>(control(at(ap::Param::FeelAmount)), ""));
		// FREE'S PHASE in the Free feels (backlog QA): a digit box, as the Pattern device's. Its Fine is the window's.
		feel_->addFollower(at(ap::Param::FreePhase), std::make_unique<ValueBox>(control(at(ap::Param::FreePhase)), ""));
		feel_->setFaceDiameter(kFaceDiameter);
		addChild(feel_.get());
		wobble_ = std::make_unique<ValueBox>(control(at(ap::Param::Wobble)), "Wobble");
		addChild(wobble_.get());
	}
	latch_ = std::make_unique<SwitchChip>(control(at(ap::Param::Latch)), "Latch");
	addChild(latch_.get());
	rate_ = std::make_unique<Knob>(control(at(ap::Param::Rate)), "Rate", notifier_);
	gate_ = std::make_unique<Knob>(control(at(ap::Param::Gate)), "Gate", notifier_);
	octaves_ = std::make_unique<Knob>(control(at(ap::Param::Octaves)), "Oct", notifier_); // "Octaves" truncates beside the Feel cell
	lane_ = std::make_unique<ArpLane>(host_, device_);
	addChild(lane_.get());
	for (Knob* knob : { rate_.get(), gate_.get(), octaves_.get() }) {
		knob->setFaceDiameter(kFaceDiameter);
		knob->setModulation(mod_, history_);
		addChild(knob);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	layoutControls();
}

ArpCard::~ArpCard() = default;

DeviceControl& ArpCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, ap::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

device::Modulatable* ArpCard::enableModulatable() {
	return &control(at(ap::Param::Enabled));
}

void ArpCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* knob : { rate_.get(), gate_.get(), octaves_.get() }) {
		knob->setModulation(mod, history);
	}
}

std::vector<size_t> ArpCard::addressedParamsForTest() const {
	std::vector<size_t> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(road->control());
		}
	};
	add(&order_->control());
	add(&timeBase_->control());
	add(&feel_->select().control());
	out.push_back(at(ap::Param::FeelAmount));
	out.push_back(at(ap::Param::FreePhase));
	add(&wobble_->control());
	add(&latch_->control());
	for (const Knob* knob : { rate_.get(), gate_.get(), octaves_.get() }) {
		add(knob->control());
	}
	return out;
}

std::vector<int> ArpCard::laneKeysForTest() const {
	return lane_->keysForTest();
}

float ArpCard::openHeight(size_t outputRows) {
	// THE FEEL'S ROW UNDER THE KNOBS, Feel and Wobble side by side (Wobble belongs on the card) — the chain
	// unit's layout everywhere, so the knobs keep their width.
	return kCardHeadHeight + bodyHeight() + controlsHeight() + kGap + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float ArpCard::laneHeightAt(float cardWidth) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	// IN A HORIZONTAL CHAIN the head is the left spine, and the Feel's cell takes a row of its own under the knobs (`layoutControls`).
	if (laneAxis() == LaneAxis::Horizontal) {
		return bodyHeight() + controlsHeight() + kGap + outputStripHeightAt(cardWidth);
	}
	return kCardHeadHeight + bodyHeight() + controlsHeight() + kGap + outputStripHeightAt(cardWidth); // the Feel's row too
}

void ArpCard::resized() {
	layoutControls();
}

void ArpCard::foldedChanged() {
	layoutControls();
}

visage::Bounds ArpCard::laneBounds() const {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	return { left, bodyTopEdge() + kGap + ValueBox::kHeight + kGap, inner, kLaneHeight };
}

void ArpCard::layoutControls() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const bool open = !folded();

	float y = bodyTopEdge() + kGap;
	const float latchWidth = std::min(52.0f, inner * 0.22f);
	const float boxes = std::max(0.0f, inner - latchWidth - 2.0f * kGap);
	const float half = std::max(0.0f, (boxes - kGap) / 2.0f);
	order_->setBounds(left, y, half, ValueBox::kHeight);
	timeBase_->setBounds(left + half + kGap, y, half, ValueBox::kHeight);
	latch_->setBounds(left + inner - latchWidth, y + 0.5f * (ValueBox::kHeight - SwitchChip::kHeight), latchWidth,
	                  SwitchChip::kHeight);
	for (visage::Frame* f : std::initializer_list<visage::Frame*> { order_.get(), timeBase_.get(), latch_.get() }) {
		f->setVisible(open);
	}
	const visage::Bounds lane = laneBounds();
	lane_->setBounds(lane.x(), lane.y(), lane.width(), lane.height());
	lane_->setVisible(open);
	y = lane.bottom() + kGap;

	Knob* row[] = { rate_.get(), gate_.get(), octaves_.get() };
	// THE FEEL'S CELL TAKES THE ROOM ITS WIDEST NAME NEEDS ("Groove 16"); the three knobs share the rest. IN A HORIZONTAL
	// CHAIN the card is a unit wide, which left the knobs too narrow for their captions ("RA…", "G…"): the cell takes the
	// row under them instead, and the knobs the whole width.
	// AND SINCE WOBBLE JOINED THE FEEL, EVERYWHERE: beside the knobs it left them no room for their
	// captions (captured, 2026-11-08); in its own row Feel and Wobble sit side by side and the knobs keep the whole width.
	const bool feelBelow = true;
	// WOBBLE at the Feel cell's right, a small two-line box: in the row's last columns, or at the end
	// of the Feel's own row in a horizontal chain.
	const float wobbleW = std::min(kWobbleWidth, std::max(0.0f, inner * 0.3f));
	const float feelW = feelBelow ? 0.0f
	                              : std::min(std::max(feel_->minimumWidth(), (inner - 3.0f * kRowGap) / 4.0f), inner * 0.45f);
	const float tail = feelBelow ? 0.0f : wobbleW + kRowGap;
	const float w = std::max(0.0f, (inner - feelW - tail - (feelBelow ? 2.0f : 3.0f) * kRowGap) / 3.0f);
	float x = left;
	for (Knob* knob : row) {
		knob->setBounds(x, y, w, controlsHeight());
		knob->setVisible(open);
		x += w + kRowGap;
	}
	if (feelBelow) {
		x = left;
		y += controlsHeight() + kGap;
	}
	// THE FEEL'S CELL, the last column (or its own row); its Amount box is shown only where the feel has one (Straight has none).
	feel_->setBounds(x, y, std::max(0.0f, left + inner - wobbleW - kRowGap - x), controlsHeight());
	feel_->setVisible(open);
	wobble_->setBounds(left + inner - wobbleW, y + 0.5f * (controlsHeight() - ValueBox::kHeight), wobbleW, ValueBox::kHeight);
	wobble_->setVisible(open);
	(void)feel_->refresh();
}

bool ArpCard::updateContent() {
	if (folded()) {
		return false;
	}
	// THE FEEL MAY HAVE MOVED: its cell shows its Amount's box only where the feel has one.
	const bool feelMoved = feel_->refresh();
	return lane_->poll() || feelMoved;
}

void ArpCard::drawContent(visage::Canvas&) {
	// NOTHING OF ITS OWN: the step lane is a frame of its own (`ArpLane`), which the window holds too.
}

//------------------------------------------------------------------------------------------------//
// The step lane                                                                                  //
//------------------------------------------------------------------------------------------------//

ArpLane::ArpLane(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {}

float ArpLane::fieldOf(ap::Param p) const {
	const size_t c = at(p);
	return c < host_.fieldsPresentAt(device_) ? host_.modulatedFieldAt(device_, c)
	                                          : static_cast<float>(ap::kParamDescs[c].def);
}

bool ArpLane::poll() {
	bool moved = false;
	std::array<device::RecentEvent, device::kRecentEvents> recent {};
	const size_t n = host_.recentEventsAt(device_, recent.data(), recent.size());
	const uint32_t newest = n > 0 ? recent[0].ordinal : 0;
	if (n != count_ || newest != newest_) {
		// NEWEST FIRST from the device; the lane draws oldest first, the newest at the right edge as the playhead.
		count_ = std::min(n, kSteps);
		for (size_t i = 0; i < count_; ++i) {
			steps_[i] = recent[count_ - 1 - i];
		}
		newest_ = newest;
		moved = true;
	}
	const float gate = fieldOf(ap::Param::Gate);
	const float feel = fieldOf(ap::Param::Feel);
	const float amount = fieldOf(ap::Param::FeelAmount);
	const float phase = fieldOf(ap::Param::FreePhase);
	const float wobble = fieldOf(ap::Param::Wobble);
	const float timeBase = fieldOf(ap::Param::TimeBase);
	const float rate = fieldOf(ap::Param::Rate);
	if (gate != gate_ || feel != feel_ || amount != amount_ || phase != phase_ || wobble != wobble_ || timeBase != timeBase_
	    || rate != rate_) {
		gate_ = gate;
		feel_ = feel;
		amount_ = amount;
		phase_ = phase;
		wobble_ = wobble;
		timeBase_ = timeBase;
		rate_ = rate;
		moved = true;
	}
	if (moved) {
		redraw();
	}
	return moved;
}

std::vector<int> ArpLane::keysForTest() const {
	std::vector<int> keys;
	for (size_t i = 0; i < count_; ++i) {
		keys.push_back(static_cast<int>(std::lround(steps_[i].value * 127.0f)));
	}
	return keys;
}

void ArpLane::draw(visage::Canvas& canvas) {
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	// THE HOUSE PLOT GROUND, as the LFO's, the S&H's and the Follower's plots: `kSurface` vanished into the card.
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	if (count_ == 0) {
		canvas.setColor(kMuted);
		canvas.text("hold a chord: its steps play here", font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, w, h);
		return;
	}
	// THE PITCH RANGE IN VIEW, so a chord in any register fills the lane.
	float lo = 1.0f;
	float hi = 0.0f;
	for (size_t i = 0; i < count_; ++i) {
		lo = std::min(lo, steps_[i].value);
		hi = std::max(hi, steps_[i].value);
	}
	const float span = std::max(hi - lo, 12.0f / 127.0f);
	const float pad = 3.0f;
	const float column = (w - 2.0f * pad) / static_cast<float>(kSteps);
	const float barHeight = std::max(3.0f, (h - 2.0f * pad) / 14.0f);
	const float share = std::clamp(gate_ / static_cast<float>(ap::kGateMax), 0.0f, 1.0f) * 2.0f;
	const size_t first = kSteps - count_;
	// THE FELT STEPS SIT WHERE THEY PLAYED: an event's ordinal is its grid slot, moved by the device's own function.
	ap::Settings s;
	s.timeBase = static_cast<size_t>(std::lround(timeBase_));
	s.rate = rate_;
	const ap::humanize::Feel feel = ap::Arp::feelOf(static_cast<size_t>(std::lround(feel_)), static_cast<double>(amount_),
	                                                   static_cast<double>(phase_), static_cast<double>(wobble_));
	const double beats = s.synced() ? ap::stepBeats(s) : 0.0;
	for (size_t i = 0; i < count_; ++i) {
		const float shift = static_cast<float>(ap::humanize::shiftSteps(feel, steps_[i].ordinal, 0.125, beats)) * column;
		const float x = pad + column * static_cast<float>(first + i) + shift;
		const float bw = std::max(2.0f, std::min(share * column, w - pad - x) - 1.0f);
		const float y = h - pad - barHeight - (steps_[i].value - lo) / span * (h - 2.0f * pad - barHeight);
		canvas.setColor(i + 1 == count_ ? kAccent : kMuted);
		canvas.roundedRectangle(x, y, bw, barHeight, 1.5f);
	}
}

bool ArpCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < ap::kDescriptor.outputs.size(); ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool ArpCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
