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

#include "plugin/ui/stepseq_card.h"

#include "device/extra_state.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace feathers::ui {

namespace {

namespace sq = feathers::mod::stepseq;

/// **THE PATTERN BEFORE AND AFTER ONE ACT** — a drag, a lock, a gate, a Randomise. One value moved from one to another,
/// so one record, written back the way the grid writes (`StepGrid::writePattern`), only the floats that differ.
class StepsEdit final : public EditRecord {
public:
	StepsEdit(backplane::DeviceHost& host, DeviceInstanceId id, sq::Pattern before, sq::Pattern after, const char* verb)
	    : host_(host), device_(id), before_(before), after_(after), verb_(verb) {}
	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return verb_; }

private:
	void write(const sq::Pattern& p) const {
		std::array<float, sq::kParamCount + sq::kExtraFloats> row {};
		sq::writePattern(p, row.data());
		for (size_t i = sq::kParamCount; i < row.size(); ++i) {
			host_.setField(device_, i, row[i]);
		}
	}
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	sq::Pattern before_;
	sq::Pattern after_;
	const char* verb_;
};

constexpr uint32_t kLockColour = 0xffE0B060u;

} // namespace

//------------------------------------------------------------------------------------------------//
// The grid                                                                                       //
//------------------------------------------------------------------------------------------------//

StepGrid::StepGrid(backplane::DeviceHost& host, DeviceInstanceId id, EditHistory* history)
    : host_(host), device_(id), history_(history) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE TABS PICK THE LANE: all five on the face, so nothing is applied to find out what they are.
		for (size_t l = 0; l < static_cast<size_t>(sq::Lane::kCount); ++l) {
			if (tabBounds(static_cast<sq::Lane>(l)).contains(e.position)) {
				setLane(static_cast<sq::Lane>(l));
				return;
			}
		}
		const int step = stepAt(e.position.x);
		if (step < 0) {
			return;
		}
		if (e.isRightButton()) {
			showStepMenu(static_cast<size_t>(step), e.position);
			return;
		}
		before_ = pattern();
		sq::Pattern p = before_;
		sq::Step& st = p.step[static_cast<size_t>(step)];
		lastStep_ = step;
		if (e.position.y < lockTop() + kLockHeight) {
			drag_ = Drag::Lock;
			paintLock_ = !st.locked;
			st.locked = paintLock_;
			writePattern(p);
		}
		else if (e.position.y >= gateTop()) {
			drag_ = Drag::Gate;
			paintGate_ = st.gate == sq::Gate::Off ? sq::Gate::On : sq::Gate::Off;
			st.gate = paintGate_;
			writePattern(p);
		}
		else {
			drag_ = Drag::Paint;
			paintAt(e.position, true);
		}
		redraw();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (drag_ == Drag::None) {
			return;
		}
		if (drag_ == Drag::Paint) {
			paintAt(e.position, false);
		}
		else {
			const int step = stepAt(e.position.x);
			if (step >= 0 && step != lastStep_) {
				sq::Pattern p = pattern();
				const int lo = std::min(step, lastStep_);
				const int hi = std::max(step, lastStep_);
				for (int s = lo; s <= hi; ++s) {
					if (drag_ == Drag::Lock) {
						p.step[static_cast<size_t>(s)].locked = paintLock_;
					}
					else {
						p.step[static_cast<size_t>(s)].gate = paintGate_;
					}
				}
				lastStep_ = step;
				writePattern(p);
			}
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (drag_ == Drag::None) {
			return;
		}
		const Drag was = drag_;
		drag_ = Drag::None;
		finishGesture(was == Drag::Lock ? "Lock" : was == Drag::Gate ? "Gate" : "Steps");
	};
	installTooltipHover(*this, *this);
}

size_t StepGrid::steps() const {
	const size_t i = sq::at(sq::Param::Steps);
	const float v = i < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, i)
	                                                   : static_cast<float>(sq::kParamDescs[i].def);
	return static_cast<size_t>(std::clamp(std::lround(v), 1L, static_cast<long>(sq::kMaxSteps)));
}

sq::Pattern StepGrid::pattern() const {
	const size_t present = host_.fieldsPresentAt(device_);
	std::array<float, sq::kParamCount + sq::kExtraFloats> row {};
	for (size_t i = 0; i < row.size() && i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return sq::patternOf(row.data(), std::min(present, row.size()));
}

sq::Pattern StepGrid::writePattern(const sq::Pattern& wanted) {
	// A HAND'S STEPS GET A FRESH BASIS (`ModOps::extraStateBasis`, backlog MX): the device drops its Turing rewrites for
	// them, and a snapshot it took before this edit can never land over it. A write that moves no step is no edit.
	sq::Pattern p = wanted;
	const sq::Pattern now = pattern();
	p.basis = now.basis;
	if (!(p == now)) {
		p.basis = device::freshExtraStateBasis();
	}
	std::array<float, sq::kParamCount + sq::kExtraFloats> row {};
	sq::writePattern(p, row.data());
	const size_t present = host_.fieldsPresentAt(device_);
	for (size_t i = sq::kParamCount; i < row.size(); ++i) {
		if (i >= present || std::bit_cast<uint32_t>(host_.fieldAt(device_, i)) != std::bit_cast<uint32_t>(row[i])) {
			host_.setField(device_, i, row[i]);
		}
	}
	redraw();
	return p;
}

void StepGrid::setLane(sq::Lane lane) {
	lane_ = lane;
	redraw();
	if (laneChanged_) {
		laneChanged_();
	}
}

void StepGrid::finishGesture(const char* verb) {
	const sq::Pattern after = pattern();
	if (history_ != nullptr && !(after == before_)) {
		history_->record(std::make_unique<StepsEdit>(host_, device_, before_, after, verb));
	}
	lastStep_ = -1;
}

visage::Bounds StepGrid::tabBounds(sq::Lane lane) const {
	const float w = width() / static_cast<float>(sq::Lane::kCount);
	return { w * static_cast<float>(lane), 0.0f, w, kTabHeight };
}

visage::Bounds StepGrid::barsBounds() const {
	return { 0.0f, kTabHeight + kLockHeight, width(), kBarsHeight };
}

visage::Bounds StepGrid::stepBounds(size_t step) const {
	const float w = width() / static_cast<float>(steps());
	return { w * static_cast<float>(step), kTabHeight, w, kHeight - kTabHeight };
}

int StepGrid::stepAt(float x) const {
	const size_t n = steps();
	if (!(width() > 0.0f)) {
		return -1;
	}
	const int s = static_cast<int>(std::floor(x / (width() / static_cast<float>(n))));
	return std::clamp(s, 0, static_cast<int>(n) - 1);
}

void StepGrid::paintAt(visage::Point p, bool first) {
	const visage::Bounds bars = barsBounds();
	const float unit = std::clamp(1.0f - (p.y - bars.y()) / std::max(bars.height(), 1.0f), 0.0f, 1.0f);
	const int step = stepAt(p.x);
	if (step < 0) {
		return;
	}
	sq::Pattern pat = pattern();
	// A FAST DRAG SKIPS STEPS BETWEEN TWO EVENTS; the line between them is what the hand drew across them.
	const int from = first ? step : lastStep_;
	const float fromUnit = first ? unit : lastUnit_;
	const int lo = std::min(from, step);
	const int hi = std::max(from, step);
	for (int s = lo; s <= hi; ++s) {
		const float t = hi == lo ? 1.0f : static_cast<float>(s - from) / static_cast<float>(step - from);
		sq::setLaneValue(&pat.step[static_cast<size_t>(s)], lane_, fromUnit + (unit - fromUnit) * t);
	}
	lastStep_ = step;
	lastUnit_ = unit;
	writePattern(pat);
}

void StepGrid::showStepMenu(size_t step, visage::Point at) {
	// ALL THREE GATES ON THE FACE OF THE MENU, the current one checked, and the lock beside them.
	const sq::Pattern p = pattern();
	const sq::Step& st = p.step[step];
	visage::PopupMenu menu;
	for (size_t g = 0; g < static_cast<size_t>(sq::Gate::kCount); ++g) {
		menu.addOption(static_cast<int>(g), sq::kGateNames[g]).select(static_cast<size_t>(st.gate) == g);
	}
	menu.addOption(10, st.locked ? "Unlock" : "Lock");
	const std::weak_ptr<void> alive = alive_;
	menu.onSelection() = [this, alive, step](int id) {
		if (alive.expired()) {
			return;
		}
		before_ = pattern();
		sq::Pattern next = before_;
		if (id >= 0 && id < static_cast<int>(sq::Gate::kCount)) {
			next.step[step].gate = static_cast<sq::Gate>(id);
		}
		else if (id == 10) {
			next.step[step].locked = !next.step[step].locked;
		}
		writePattern(next);
		finishGesture(id == 10 ? "Lock" : "Gate");
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

bool StepGrid::poll() {
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase)) {
		ordinal = mod::Playhead::kIdle;
	}
	if (ordinal == drawnOrdinal_) {
		return false;
	}
	drawnOrdinal_ = ordinal;
	redraw();
	return true;
}

const char* StepGrid::tooltipAt(visage::Point local) const {
	if (local.y < kTabHeight) {
		return kTabsTip;
	}
	if (local.y < kTabHeight + kLockHeight) {
		return kLockTip;
	}
	if (local.y >= gateTop()) {
		return kGateTip;
	}
	return kBarsTip;
}

void StepGrid::draw(visage::Canvas& canvas) {
	const float w = width();
	const sq::Pattern p = pattern();
	const size_t n = steps();
	const float stepW = w / static_cast<float>(n);
	const visage::Bounds bars = barsBounds();

	// THE TABS.
	const visage::Font& face = font(kKnobLabelSize);
	for (size_t l = 0; l < static_cast<size_t>(sq::Lane::kCount); ++l) {
		const visage::Bounds b = tabBounds(static_cast<sq::Lane>(l));
		const bool on = static_cast<size_t>(lane_) == l;
		if (on) {
			canvas.setColor(withAlpha(kAccent, 0.25f));
			canvas.roundedRectangle(b.x() + 1.0f, b.y(), b.width() - 2.0f, b.height(), 3.0f);
		}
		canvas.setColor(on ? kInk : kMuted);
		canvas.text(sq::kLaneNames[l], face, visage::Font::kCenter, b.x(), b.y(), b.width(), b.height());
	}

	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, bars.y(), w, bars.height(), 3.0f);

	const uint32_t playing = drawnOrdinal_;
	for (size_t s = 0; s < n; ++s) {
		const sq::Step& st = p.step[s];
		const float x = stepW * static_cast<float>(s);
		const float inner = std::max(1.0f, stepW - 1.0f);
		const bool here = playing == static_cast<uint32_t>(s);
		if (here) {
			canvas.setColor(withAlpha(kInk, 0.10f));
			canvas.fill(x, bars.y(), inner, bars.height());
		}
		// THE BAR: the lane's value from the floor, dimmer for a step that rests (its gate is off).
		const float v = sq::laneValue(st, lane_);
		const float h = std::max(1.0f, v * (bars.height() - 2.0f));
		const float alpha = st.gate == sq::Gate::Off && lane_ != sq::Lane::Gate ? 0.35f : here ? 1.0f : 0.8f;
		canvas.setColor(withAlpha(lane_ == sq::Lane::Value ? kValue : kAccent, alpha));
		canvas.fill(x + 0.5f, bars.bottom() - 1.0f - h, std::max(1.0f, inner - 1.0f), h);
		// THE LOCK.
		// AN OPEN ONE IS A FAINT OUTLINE, so the strip says it is there to be pressed.
		if (st.locked) {
			canvas.setColor(kLockColour);
			canvas.fill(x + inner * 0.25f, lockTop() + 1.5f, inner * 0.5f, kLockHeight - 3.0f);
		}
		else {
			canvas.setColor(withAlpha(kLine, 0.5f));
			canvas.roundedRectangleBorder(x + inner * 0.25f, lockTop() + 1.5f, inner * 0.5f, kLockHeight - 3.0f, 1.0f,
			                              1.0f);
		}
		// THE GATE: a filled pad on, an outline at rest, a bar joined to the step before for a tie.
		const float gy = gateTop() + 2.0f;
		const float gh = kGateHeight - 3.0f;
		switch (st.gate) {
		case sq::Gate::On:
			canvas.setColor(withAlpha(kInk, 0.75f));
			canvas.fill(x + 1.0f, gy, std::max(1.0f, inner - 2.0f), gh);
			break;
		case sq::Gate::Tie:
			canvas.setColor(withAlpha(kInk, 0.75f));
			canvas.fill(x - 1.0f, gy + gh * 0.3f, inner + 1.0f, gh * 0.4f);
			break;
		default:
			canvas.setColor(withAlpha(kLine, 0.9f));
			canvas.roundedRectangleBorder(x + 1.0f, gy, std::max(1.0f, inner - 2.0f), gh, 1.0f, 1.0f);
			break;
		}
	}
}

//------------------------------------------------------------------------------------------------//
// The button                                                                                     //
//------------------------------------------------------------------------------------------------//

ActButton::ActButton(std::string label, const char* tooltip, std::function<void()> act)
    : label_(std::move(label)), tooltip_(tooltip), act_(std::move(act)) {
	onMouseDown() = [this](const visage::MouseEvent&) { press(); };
	installTooltipHover(*this, *this);
}

void ActButton::draw(visage::Canvas& canvas) {
	canvas.setColor(withAlpha(kAccent, 0.22f));
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 4.0f);
	canvas.setColor(kInk);
	canvas.text(label_, font(kControlSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
}

//------------------------------------------------------------------------------------------------//
// The card                                                                                       //
//------------------------------------------------------------------------------------------------//

StepSeqCard::StepSeqCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier, ModState* mod,
                         EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	drawn_.fill(std::numeric_limits<double>::quiet_NaN());
	grid_ = std::make_unique<StepGrid>(host_, device_, history_);

	const auto pad = [this](sq::Param p, uint32_t shift, uint32_t mask) {
		return std::make_unique<TrigPad>(control(sq::at(p)), [this, shift, mask](uint32_t& fires) {
			uint32_t word = 0;
			if (!host_.trigFiresAt(device_, word)) {
				return false;
			}
			fires = (word >> shift) & mask; // the Advance's count, the Reset's or Randomise's (`Device::fireWord`)
			return true;
		});
	};
	advancePad_ = pad(sq::Param::Trig, sq::kFireAdvanceShift, sq::kFireAdvanceMask);
	resetPad_ = pad(sq::Param::Reset, sq::kFireResetShift, sq::kFireResetMask);
	// AND EACH LANE'S RANDOMISE (backlog NT: 2026-10-18): its own pad and its own button, on that lane's page.
	// A fire is a press of that lane's button, made on the audio thread, whichever page is shown.
	for (size_t l = 0; l < sq::kLaneCount; ++l) {
		const auto lane = static_cast<sq::Lane>(l);
		randomisePads_[l] = pad(sq::randomiseParam(lane), sq::fireRandomShift(lane), sq::kFireRandomMask);
		randomiseButtons_[l] = std::make_unique<ActButton>(
		    "Randomise", "Moves this page's lane in every unlocked step by Rand Amt, drawn from Seed. One undo step",
		    [this, lane] { (void)randomise(lane); });
	}
	// THE RESET PAD CARRIES THE NORMAL, AND IT IS RETRIG (backlog NU, approved 2026-10-18): a note restarting the pattern
	// is a note firing Reset, which is exactly what `retrig` already stores — so the pad's "Notes fire Trig" row drives
	// that switch rather than a second key for the same fact. The Advance has no normal: no note advances it.
	resetPad_->setNormal({ &control(sq::at(sq::Param::Retrig)) });

	const auto box = [this](sq::Param p, const char* caption) {
		auto b = std::make_unique<ValueBox>(control(sq::at(p)), caption);
		return b;
	};
	steps_ = box(sq::Param::Steps, "Steps");
	direction_ = box(sq::Param::Direction, "Direction");
	clock_ = box(sq::Param::Clock, "Clock");
	timeBase_ = box(sq::Param::TimeBase, "Base");
	seed_ = box(sq::Param::Seed, "Seed");
	// "RAND AMT", NOT "AMOUNT" (backlog NT): it is how far Randomise moves, and the Amount beside the outputs is the
	// output's scale, as on the LFO.
	amount_ = box(sq::Param::RandomAmount, "Rand Amt");
	outAmount_ = box(sq::Param::Amount, "Amount");

	const auto knob = [this](sq::Param p, const char* caption) {
		auto k = std::make_unique<Knob>(control(sq::at(p)), caption, notifier_);
		k->setFaceDiameter(kKnobDiameter);
		k->setModulation(mod_, history_);
		return k;
	};
	rate_ = knob(sq::Param::Rate, "Rate");
	gateLength_ = knob(sq::Param::GateLength, "Gate");
	lock_ = knob(sq::Param::TuringLock, "Lock");

	turing_ = std::make_unique<SwitchChip>(control(sq::at(sq::Param::Turing)), "Turing");
	bipolar_ = std::make_unique<SwitchChip>(control(sq::at(sq::Param::Bipolar)), "Bipolar");
	retrig_ = std::make_unique<SwitchChip>(control(sq::at(sq::Param::Retrig)), "Retrig");
	poly_ = std::make_unique<SwitchChip>(control(sq::at(sq::Param::Poly)), "Poly");

	// A PAGE OPENED SHOWS ITS OWN LANE'S PAIR.
	grid_->onLaneChanged([this] { applyRelevance(); });

	for (visage::Frame* w : widgets()) {
		addChild(w);
	}
	if (mod_ != nullptr) {
		watches_.push_back(mod_->watch(advancePad_.get()));
		watches_.push_back(mod_->watch(resetPad_.get()));
		for (const auto& p : randomisePads_) {
			watches_.push_back(mod_->watch(p.get()));
		}
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE PRESS IS THE CARD'S TO HAND BACK — `MathCard`'s note.
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	applyRelevance();
}

StepSeqCard::~StepSeqCard() {
	alive_.reset();
	watches_.clear();
}

std::vector<visage::Frame*> StepSeqCard::widgets() const {
	std::vector<visage::Frame*> out { grid_.get(),     advancePad_.get(), resetPad_.get(), steps_.get(),
		                              direction_.get(), clock_.get(),     timeBase_.get(), rate_.get(),
		                              gateLength_.get(), lock_.get(),     seed_.get(),     amount_.get(),
		                              turing_.get(),    bipolar_.get(),   retrig_.get(),   poly_.get(),
		                              outAmount_.get() };
	for (size_t l = 0; l < sq::kLaneCount; ++l) {
		out.push_back(randomiseButtons_[l].get());
		out.push_back(randomisePads_[l].get());
	}
	return out;
}

DeviceControl& StepSeqCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, sq::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

device::Modulatable* StepSeqCard::enableModulatable() {
	return &control(sq::at(sq::Param::Enabled));
}

void StepSeqCard::setModulation(ModState* mod, EditHistory* history) {
	watches_.clear();
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* k : { rate_.get(), gateLength_.get(), lock_.get() }) {
		k->setModulation(mod, history);
	}
	grid_->setHistory(history);
	if (mod_ != nullptr) {
		watches_.push_back(mod_->watch(advancePad_.get()));
		watches_.push_back(mod_->watch(resetPad_.get()));
		for (const auto& p : randomisePads_) {
			watches_.push_back(mod_->watch(p.get()));
		}
	}
}

bool StepSeqCard::randomise() {
	return randomise(grid_->lane());
}

bool StepSeqCard::randomise(sq::Lane lane) {
	const sq::Pattern before = grid_->pattern();
	const auto field = [this](sq::Param p) { return control(sq::at(p)).value(); };
	const sq::Pattern after = sq::randomise(before, grid_->steps(), lane,
	                                        static_cast<float>(field(sq::Param::RandomAmount)),
	                                        static_cast<int>(std::lround(field(sq::Param::Seed))));
	if (after == before) {
		return false;
	}
	const sq::Pattern written = grid_->writePattern(after);
	if (history_ != nullptr) {
		history_->record(std::make_unique<StepsEdit>(host_, device_, before, written, "Randomise"));
	}
	return true;
}

std::vector<size_t> StepSeqCard::addressedParamsForTest() const {
	std::vector<size_t> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(road->control());
		}
	};
	add(&advancePad_->control());
	add(&resetPad_->control());
	for (const auto& p : randomisePads_) {
		add(&p->control());
	}
	for (ValueBox* b : { steps_.get(), direction_.get(), clock_.get(), timeBase_.get(), seed_.get(), amount_.get(),
	                     outAmount_.get() }) {
		add(&b->control());
	}
	for (Knob* k : { rate_.get(), gateLength_.get(), lock_.get() }) {
		add(k->control());
	}
	for (SwitchChip* c : { turing_.get(), bipolar_.get(), retrig_.get(), poly_.get() }) {
		add(&c->control());
	}
	return out;
}

float StepSeqCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float StepSeqCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

void StepSeqCard::applyRelevance() {
	const auto live = [this](sq::Param p) {
		const device::ParamDesc& d = sq::kParamDescs[sq::at(p)];
		return d.gateControl < 0 || device::relevantFor(d, control(static_cast<size_t>(d.gateControl)).value());
	};
	const bool open = !folded();
	for (visage::Frame* w : widgets()) {
		w->setVisible(open);
	}
	timeBase_->setVisible(open && live(sq::Param::TimeBase));
	rate_->setVisible(open && live(sq::Param::Rate));
	lock_->setVisible(open && live(sq::Param::TuringLock));
	poly_->setVisible(open && live(sq::Param::Poly));
	// ONLY THE PAGE SHOWN HAS ITS RANDOMISE ON THE FACE; the others keep their routes, which their pads show when opened.
	for (size_t l = 0; l < sq::kLaneCount; ++l) {
		const bool shown = open && static_cast<size_t>(grid_->lane()) == l;
		randomiseButtons_[l]->setVisible(shown);
		randomisePads_[l]->setVisible(shown);
	}
}

void StepSeqCard::resized() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	float y = bodyTopEdge() + kGap;

	// THE GRID, with the two pads at its left: Advance level with the bars' top, Reset under it.
	grid_->setBounds(left + kPadColumn, y, std::max(0.0f, inner - kPadColumn), StepGrid::kHeight);
	const float padX = left + (kPadColumn - TrigPad::kDiameter) * 0.5f - 2.0f;
	const float barsTop = y + StepGrid::kTabHeight + StepGrid::kLockHeight;
	advancePad_->setBounds(padX, barsTop + 4.0f, TrigPad::kDiameter, TrigPad::kDiameter);
	resetPad_->setBounds(padX, barsTop + StepGrid::kBarsHeight - TrigPad::kDiameter - 4.0f, TrigPad::kDiameter,
	                     TrigPad::kDiameter);
	y += StepGrid::kHeight + kGap;

	const float quarter = std::max(0.0f, (inner - 3.0f * kGap) / 4.0f);
	const auto column = [&](int i) { return left + static_cast<float>(i) * (quarter + kGap); };

	// THE STEPS' SHAPE IN TIME: how many, which way, what moves it on, and how long one is.
	steps_->setBounds(column(0), y, quarter, ValueBox::kHeight);
	direction_->setBounds(column(1), y, quarter, ValueBox::kHeight);
	clock_->setBounds(column(2), y, quarter, ValueBox::kHeight);
	timeBase_->setBounds(column(3), y, quarter, ValueBox::kHeight);
	y += ValueBox::kHeight + kGap;

	rate_->setBounds(column(0), y, quarter, knobRowHeight());
	gateLength_->setBounds(column(1), y, quarter, knobRowHeight());
	turing_->setBounds(column(2), y + (kKnobDiameter - SwitchChip::kHeight) * 0.5f, quarter, SwitchChip::kHeight);
	lock_->setBounds(column(3), y, quarter, knobRowHeight());
	y += knobRowHeight() + kGap;

	// THE ACT AND WHAT IT DRAWS FROM.
	// THE SHOWN PAGE'S PAD AT ITS BUTTON'S LEFT (backlog NT) — all five pairs in one place, one visible at a time.
	const float buttonX = column(0) + TrigPad::kDiameter + 3.0f;
	for (size_t l = 0; l < sq::kLaneCount; ++l) {
		randomisePads_[l]->setBounds(column(0), y + (ValueBox::kHeight - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
		                             TrigPad::kDiameter);
		randomiseButtons_[l]->setBounds(buttonX, y + (ValueBox::kHeight - 18.0f) * 0.5f,
		                                std::max(0.0f, column(2) - kGap - buttonX), 18.0f);
	}
	amount_->setBounds(column(2), y, quarter, ValueBox::kHeight);
	seed_->setBounds(column(3), y, quarter, ValueBox::kHeight);
	y += ValueBox::kHeight + kGap;

	// THE OUTPUT'S ROW: the switches centred on the Amount's box, which scales the Value output (backlog NT).
	const float chipY = y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f;
	retrig_->setBounds(column(0), chipY, quarter, SwitchChip::kHeight);
	poly_->setBounds(column(1), chipY, quarter, SwitchChip::kHeight);
	outAmount_->setBounds(column(2), y, quarter, ValueBox::kHeight);
	bipolar_->setBounds(column(3), chipY, quarter, SwitchChip::kHeight);
	applyRelevance();
}

void StepSeqCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	const visage::Font& face = font(kKnobLabelSize);
	canvas.setColor(kMuted);
	for (const auto& [pad, caption] : { std::pair { advancePad_.get(), "Adv" }, std::pair { resetPad_.get(), "Rst" } }) {
		canvas.text(caption, face, visage::Font::kCenter, pad->x() - 4.0f, pad->y() + pad->height() + 1.0f,
		            pad->width() + 8.0f, kKnobLabelSize + 2.0f);
	}
}

void StepSeqCard::foldedChanged() {
	applyRelevance();
}

bool StepSeqCard::updateContent() {
	// POLLED, for MATH's reason: an undo, a load or host automation moves a field and nothing tells the card.
	bool told = advancePad_->poll();
	told = resetPad_->poll() || told;
	for (const auto& p : randomisePads_) {
		told = p->poll() || told;
	}
	told = grid_->poll() || told;
	const size_t present = host_.fieldsPresentAt(device_);
	bool moved = false;
	for (size_t i = 0; i < drawn_.size(); ++i) {
		const double now = i < present ? static_cast<double>(host_.fieldAt(device_, i)) : 0.0;
		if (now != drawn_[i]) {
			drawn_[i] = now;
			moved = true;
		}
	}
	if (moved) {
		applyRelevance();
		for (visage::Frame* w : widgets()) {
			w->redraw();
		}
		told = true;
	}
	return told;
}

bool StepSeqCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t o = 0; o < std::size(sq::kOutputs); ++o) {
		if (mod_->source() == backplane::Source::output(device_, o)) {
			return true;
		}
	}
	return false;
}

bool StepSeqCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
