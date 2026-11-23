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

#include "plugin/ui/sample_hold_card.h"

#include "mod/samplehold/samplehold.h"
#include "plugin/ui/plot_grid.h"
#include "plugin/ui/tab_chip.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {

namespace sh = feathers::mod::samplehold;

[[nodiscard]] constexpr size_t at(sh::Param p) { return static_cast<size_t>(p); }

/// THE CAPTIONS — the short forms; the head already says Sample & Hold.
[[nodiscard]] const char* captionOf(sh::Param p) {
	switch (p) {
	case sh::Param::Source: return "Source";
	case sh::Param::Clock: return "Clock";
	case sh::Param::TimeBase: return "Base";
	case sh::Param::Rate: return "Rate";
	case sh::Param::Slew: return "Slew";
	case sh::Param::Quantize: return "Quant";
	case sh::Param::Probability: return "Prob";
	case sh::Param::Amount: return "Amt";
	case sh::Param::Input: return "Input";
	case sh::Param::Seed: return "Seed";
	case sh::Param::SeedMode: return "Free";
	case sh::Param::Bipolar: return "Bipolar";
	case sh::Param::Poly: return "Poly";
	default: return "";
	}
}

[[nodiscard]] double steadySeconds() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

SampleHoldCard::SampleHoldCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history)
    : host_(host), device_(id), mod_(mod), history_(history) {
	// **ONE FULL SET OF CONTROLS PER TAB**, each bound to its own tab's fields, all built now: a tab press shows one and
	// hides the rest, so no control is ever re-pointed.
	for (size_t t = 0; t < kTabCount; ++t) {
		TabSet& set = sets_[t];
		for (size_t i = 0; i < kBoxes.size(); ++i) {
			const sh::Param p = kBoxes[i];
			set.boxes[i] = std::make_unique<ValueBox>(controlOfTab(t, at(p)), captionOf(p));
			// A GATE'S PICK RE-LAYS THE ROWS IN THE SAME FRAME, rather than at the next poll.
			set.boxes[i]->onValueChanged([this]() {
				layoutControls();
				redraw();
			});
			addChild(set.boxes[i].get());
			// **RATE READS HZ ON THE HZ BASE** — the LFO card's rule (backlog NJ): there the number is a frequency.
			if (p == sh::Param::Rate) {
				set.boxes[i]->setReadoutFormatter(
				    [this, t](double v) {
					    sh::Settings s {};
					    s.timeBase = static_cast<size_t>(
					        std::max(0L, std::lround(host_.fieldAt(device_, fieldOf(t, sh::Param::TimeBase)))));
					    s.timeScale = static_cast<float>(v);
					    if (s.synced()) {
						    return device::formatValue(sh::kParamDescs[at(sh::Param::Rate)], v);
					    }
					    char out[32] = {};
					    std::snprintf(out, sizeof(out), "%.3gHz", sh::rateHz(s, 0.0));
					    return std::string(out);
				    },
				    true);
			}
		}
		for (size_t i = 0; i < kChips.size(); ++i) {
			set.chips[i] = std::make_unique<SwitchChip>(controlOfTab(t, at(kChips[i])), captionOf(kChips[i]));
			addChild(set.chips[i].get());
		}
		set.trigPad = std::make_unique<TrigPad>(controlOfTab(t, at(sh::Param::Trig)), [this, t](uint32_t& fires) {
			// THE FIRE COUNT THE DEVICE PUBLISHES IS THE VIEWED TAB'S, so only that pad lights from it.
			return t == tab_ && host_.trigFiresAt(device_, fires);
		});
		addChild(set.trigPad.get());
		// ITS NORMAL IS THE CLOCK ITSELF (backlog NU, approved 2026-10-18): Note is notes firing the Trig, Trig is only the
		// Trig — so the pad's "Notes fire Trig" row flips Note and Trig, and is offered only under those two clocks (under
		// Rate and Random no note samples, so there is no normal to break). No second key for the same fact.
		set.trigPad->setNormal({ &controlOfTab(t, at(sh::Param::Clock)), static_cast<double>(sh::Clock::Note),
		                         static_cast<double>(sh::Clock::Trig) });
		watches_.push_back(mod_.watch(set.trigPad.get())); // its ring is the mapping signal
	}
	watches_.push_back(mod_.watch(this));

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		if (enablePressAt(e.position)) {
			return;
		}
		// THE TAB STRIP: the On square toggles that tab, the rest of the tab selects it.
		if (const size_t tab = tabAt(e.position); tab < kTabCount) {
			if (tabOnBounds(tab).contains(e.position)) {
				toggleTabOn(tab);
			}
			else {
				selectTab(tab);
			}
		}
	};
	showTabOutputs();
	layoutControls();
}

SampleHoldCard::~SampleHoldCard() = default;

DeviceControl& SampleHoldCard::control(size_t index) const {
	return controlOfTab(tab_, index);
}

DeviceControl& SampleHoldCard::controlOfTab(size_t tab, size_t index) const {
	auto& slot = sets_[tab].controls[index];
	if (slot == nullptr) {
		slot = std::make_unique<DeviceControl>(host_, device_, sh::kDescriptor, mod::tabField(sh::kPerTab, tab, index), &mod_,
		                                      &history_);
	}
	return *slot;
}

ValueBox& SampleHoldCard::box(sh::Param p) const {
	for (size_t i = 0; i < kBoxes.size(); ++i) {
		if (kBoxes[i] == p) {
			return *cur().boxes[i];
		}
	}
	return *cur().boxes[0];
}

SwitchChip& SampleHoldCard::chip(sh::Param p) const {
	for (size_t i = 0; i < kChips.size(); ++i) {
		if (kChips[i] == p) {
			return *cur().chips[i];
		}
	}
	return *cur().chips[0];
}

device::Modulatable* SampleHoldCard::enableModulatable() {
	return &controlOfTab(0, at(sh::Param::Enabled)); // the head's switch is tab 1's
}

bool SampleHoldCard::boxLive(sh::Param p) const {
	// THE DESCRIPTOR DECIDES; THE INSTANCE SUPPLIES THE GATE'S VALUE — `LfoCard::boxIsLive`'s two lines.
	const device::ParamDesc& d = sh::kParamDescs[fieldOf(p)];
	if (d.gateControl < 0) {
		return true;
	}
	return device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

bool SampleHoldCard::inputRead() const {
	return std::lround(host_.fieldAt(device_, fieldOf(sh::Param::Source))) == static_cast<long>(sh::Source::Input);
}

float SampleHoldCard::gateKey() const {
	const auto field = [this](sh::Param p) { return std::lround(host_.fieldAt(device_, fieldOf(p))); };
	return static_cast<float>(field(sh::Param::Source) + 8 * field(sh::Param::Clock) + 64 * field(sh::Param::SeedMode));
}

float SampleHoldCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float SampleHoldCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

visage::Bounds SampleHoldCard::scopeBounds() const {
	const float left = bodyLeft() + kPadX + kTrigColumn;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX - kTrigColumn);
	// **A ROW THE GATES EMPTY IS THE SCOPE'S**: the body keeps its height, and the staircase takes what the boxes left.
	const float spare = static_cast<float>(kBoxRows - boxRowsUsed()) * (ValueBox::kHeight + kRowGap);
	return { left, bodyTopEdge() + kTabStrip + kGap, inner, kScopeHeight + spare };
}

void SampleHoldCard::resized() {
	layoutControls();
}

void SampleHoldCard::layoutControls() {
	const bool open = !folded();
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const visage::Bounds scope = scopeBounds();
	const float each = std::max(0.0f, (inner - static_cast<float>(kColumns - 1) * kRowGap) / static_cast<float>(kColumns));
	const float gridTop = scope.bottom() + kGap;
	// THE CHIPS STAY WHERE THEY ARE, whatever the scope took: from the body's top, not from the grid.
	const float chipTop = bodyTopEdge() + kTabStrip + kGap + kScopeHeight + kGap
	                      + static_cast<float>(kBoxRows) * (ValueBox::kHeight + kRowGap) - kRowGap + kGap;
	const float third = std::max(0.0f, (inner - 2.0f * kRowGap) / 3.0f);
	// EVERY TAB'S SET IS PLACED, only the selected one is shown: a card whose tab or gate changes while it is folded comes
	// back with its bounds right. The gates are asked of the SELECTED tab.
	for (size_t t = 0; t < kTabCount; ++t) {
		TabSet& set = sets_[t];
		const bool shown = open && t == tab_;
		set.trigPad->setBounds(left, scope.y() + (scope.height() - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
		                       TrigPad::kDiameter);
		set.trigPad->setVisible(shown);
		// THE GRID: the live boxes in reading order, four a row. A dead one is still placed (at the end), so a card whose
		// gate changes while folded comes back with its bounds right; it is just not drawn.
		size_t cell = 0;
		for (size_t i = 0; i < kBoxes.size(); ++i) {
			const bool live = boxLive(kBoxes[i]);
			const size_t place = live ? cell++ : kColumns * kBoxRows - 1;
			const auto column = static_cast<float>(place % kColumns);
			const auto row = static_cast<float>(place / kColumns);
			set.boxes[i]->setBounds(left + column * (each + kRowGap), gridTop + row * (ValueBox::kHeight + kRowGap), each,
			                        ValueBox::kHeight);
			set.boxes[i]->setVisible(shown && live);
		}
		// **THE INPUT IS ALWAYS ON THE FACE, GREYED WHEN NOTHING READS IT** (backlog OO: "grey when not
		// used so its easier to find"): under the noise sources the clock never samples it, so it is dimmed, refuses the
		// hand and routes, and its tooltip says why — the honesty rule, without hiding the one knob that makes S&H sample
		// something other than its own noise.
		for (size_t i = 0; i < kBoxes.size(); ++i) {
			if (kBoxes[i] == sh::Param::Input) {
				set.boxes[i]->setDisabledReason("Samples the noise — set Source to Input to sample this");
				set.boxes[i]->setEnabled(std::lround(host_.fieldAt(device_, fieldOf(t, sh::Param::Source))) ==
				                         static_cast<long>(sh::Source::Input));
			}
		}
		for (size_t i = 0; i < kChips.size(); ++i) {
			set.chips[i]->setBounds(left + static_cast<float>(i) * (third + kRowGap), chipTop, third, SwitchChip::kHeight);
			set.chips[i]->setVisible(shown);
		}
	}
	gateDrawn_ = gateKey();
}

void SampleHoldCard::foldedChanged() {
	layoutControls();
}

size_t SampleHoldCard::boxRowsUsed() const {
	size_t live = 0;
	for (const sh::Param p : kBoxes) {
		live += boxLive(p) ? 1u : 0u;
	}
	return std::clamp<size_t>((live + kColumns - 1) / kColumns, 1, kBoxRows);
}

SampleHoldCard::Reading SampleHoldCard::scopeAtForTest(size_t back) const noexcept {
	if (back >= count_) {
		return {};
	}
	return ring_[(head_ + kScopeCapacity - 1 - back) % kScopeCapacity];
}

bool SampleHoldCard::pollScope(double dtSeconds) {
	uint32_t ordinal = 0;
	float phase = 0.0f;
	float level = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase) || !host_.playheadLevelAt(device_, level)) {
		// NOTHING PUBLISHED: no reading, and no invented one.
		const bool had = count_ > 0;
		count_ = 0;
		head_ = 0;
		haveOrdinal_ = false;
		return had;
	}
	const bool taken = haveOrdinal_ && ordinal != lastOrdinal_;
	if (!haveOrdinal_) {
		glide_ = level;
	}
	// EVERY STEP SINCE THE LAST FRAME: the events whose ordinal lies after the last frame's, up to this one's. Where
	// the host keeps none (or the count went back, a restart), the frame's one reading stands for them.
	uint32_t steps = taken ? ordinal - lastOrdinal_ : 0u;
	float lo = level;
	float hi = level;
	if (taken) {
		std::array<device::RecentEvent, device::kRecentEvents> recent {};
		const uint32_t since = ordinal - lastOrdinal_;
		const size_t n = since <= 0x7FFFFFFFu ? host_.recentEventsAt(device_, recent.data(), recent.size()) : 0;
		for (size_t i = 0; i < n; ++i) {
			const uint32_t back = ordinal - recent[i].ordinal;
			if (back >= since) {
				break; // at or before the last frame's step
			}
			lo = std::min(lo, recent[i].value);
			hi = std::max(hi, recent[i].value);
		}
		if (since > 0x7FFFFFFFu) {
			steps = 1; // the count went back: a restart, one new step
		}
	}
	haveOrdinal_ = true;
	lastOrdinal_ = ordinal;
	// THE SLEW, BY THE DEVICE'S OWN LAW over the frame's real elapsed time — `mod::smoothingCoeff`'s exact pole.
	const double tau = mod::smoothingTau(host_.fieldAt(device_, fieldOf(sh::Param::Slew)));
	const double c = tau > 0.0 && dtSeconds > 0.0 ? 1.0 - std::exp(-dtSeconds / tau) : 1.0;
	glide_ += (static_cast<double>(level) - glide_) * c;
	const Reading r { level, static_cast<float>(glide_), taken, steps, lo, hi };
	const Reading last = scopeAtForTest(0);
	still_ = count_ > 0 && !taken && last.level == r.level && last.glided == r.glided ? still_ + 1 : 0;
	ring_[head_] = r;
	head_ = (head_ + 1) % kScopeCapacity;
	count_ = std::min(count_ + 1, kScopeCapacity);
	return true;
}

bool SampleHoldCard::updateContent() {
	bool told = false;
	// THE VIEW FIELD MOVED UNDER US (a patch load, an undo, a host write): follow it, with no write back.
	if (const auto viewed = static_cast<size_t>(std::clamp(std::lround(host_.fieldAt(device_, sh::kViewTabField)), 0L, static_cast<long>(kTabCount) - 1)); viewed != tab_) {
		tab_ = viewed;
		count_ = 0;
		head_ = 0;
		haveOrdinal_ = false;
		layoutControls();
		told = true;
	}
	// THE TABS' ON SQUARES move by more roads than a press (undo, a preset, automation, a route).
	unsigned onMask = 0;
	for (size_t t = 0; t < kTabCount; ++t) {
		onMask |= (tabOn(t) ? 1u : 0u) << t;
	}
	if (onMask != tabsOnDrawn_) {
		tabsOnDrawn_ = onMask;
		showTabOutputs();
		told = true;
	}
	told = cur().trigPad->poll() || told;
	if (gateKey() != gateDrawn_) {
		layoutControls();
		told = true;
	}
	const double now = steadySeconds();
	const double dt = haveClock_ ? now - lastSeconds_ : 0.0;
	haveClock_ = true;
	lastSeconds_ = now;
	(void)pollScope(dt);
	// A REPAINT ONLY WHILE THE PICTURE MOVES: once every reading the scope shows is the newest, it is a still line.
	const auto shownWidth = static_cast<size_t>(std::max(0.0f, scopeBounds().width()));
	if (count_ > 0 && still_ <= std::min(shownWidth, count_) + 1) {
		told = true;
	}
	if (told) {
		redraw();
	}
	return told;
}

void SampleHoldCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawTabs(canvas);
	const visage::Bounds box = scopeBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 4.0f, 1.0f);
	plot::drawGrid(canvas, box);

	// RAW 0..1 BOTTOM TO TOP, a few pixels inside the box; the foot keeps a strip for the taken ticks.
	constexpr float kInset = 5.0f;
	constexpr float kTickStrip = 7.0f;
	const float top = box.y() + kInset;
	const float span = std::max(0.0f, box.height() - 2.0f * kInset - kTickStrip);
	const auto yOf = [&](float v) { return top + (1.0f - std::clamp(v, 0.0f, 1.0f)) * span; };
	if (host_.fieldAt(device_, fieldOf(sh::Param::Bipolar)) >= 0.5f) {
		canvas.setColor(withAlpha(kLine, 0.5f));
		canvas.segment(box.x() + 2.0f, yOf(0.5f), box.right() - 2.0f, yOf(0.5f), 1.0f, true);
	}
	const size_t shown = std::min(count_, static_cast<size_t>(std::max(0.0f, box.width() - 4.0f)));
	const float newestX = box.right() - 3.0f;
	// THE STAIRCASE: the hold, flat between samples and a vertical at each one.
	canvas.setColor(withAlpha(kMuted, 0.8f));
	for (size_t back = 1; back < shown; ++back) {
		const float x0 = newestX - static_cast<float>(back);
		const Reading older = scopeAtForTest(back);
		const Reading newer = scopeAtForTest(back - 1);
		canvas.segment(x0, yOf(older.level), x0 + 1.0f, yOf(older.level), 1.0f, true);
		// THE VERTICAL SPANS EVERY STEP THE FRAME TOOK, from where the hold stood: one step is the plain riser.
		const float lo = std::min({ older.level, newer.level, newer.taken ? newer.lo : newer.level });
		const float hi = std::max({ older.level, newer.level, newer.taken ? newer.hi : newer.level });
		if (hi != lo) {
			canvas.segment(x0 + 1.0f, yOf(lo), x0 + 1.0f, yOf(hi), 1.0f, true);
		}
	}
	// THE OUTPUT: the slew's glide over the steps, in the modulation colour.
	canvas.setColor(kModulationL);
	for (size_t back = 1; back < shown; ++back) {
		const float x0 = newestX - static_cast<float>(back);
		canvas.segment(x0, yOf(scopeAtForTest(back).glided), x0 + 1.0f, yOf(scopeAtForTest(back - 1).glided), 1.5f,
		               true);
	}
	if (shown > 0) {
		canvas.circle(newestX - 2.5f, yOf(scopeAtForTest(0).glided) - 2.5f, 5.0f);
	}
	// TRIG OUT: a tick at the foot for every sample taken, in the trigger's own amber.
	canvas.setColor(TrigPad::kLitColour);
	const float footY = box.bottom() - kInset;
	for (size_t back = 0; back < shown; ++back) {
		if (scopeAtForTest(back).taken) {
			const float x = newestX - static_cast<float>(back);
			canvas.segment(x, footY - kTickStrip + 2.0f, x, footY, 1.5f, true);
		}
	}
}

std::vector<sh::Param> SampleHoldCard::addressedParamsForTest() const {
	std::vector<sh::Param> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(static_cast<sh::Param>(road->control() % sh::kPerTab)); // the Param, whichever tab
		}
	};
	for (const auto& b : cur().boxes) {
		add(&b->control());
	}
	for (const auto& c : cur().chips) {
		add(&c->control());
	}
	add(&cur().trigPad->control());
	return out;
}

//------------------------------------------------------------------------------------------------//
// The tabs                                                                                       //
//------------------------------------------------------------------------------------------------//

visage::Bounds SampleHoldCard::tabBounds(size_t tab) const {
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float each = (usable - kTabGap * static_cast<float>(kTabCount - 1)) / static_cast<float>(kTabCount);
	return { bodyLeft() + kPadX + static_cast<float>(tab) * (each + kTabGap), bodyTopEdge() + 2.0f, each, kTabStrip - 4.0f };
}

visage::Bounds SampleHoldCard::tabOnBounds(size_t tab) const {
	const visage::Bounds b = tabBounds(tab);
	const float side = std::min(10.0f, b.height() - 4.0f);
	return { b.x() + 4.0f, b.y() + (b.height() - side) * 0.5f, side, side };
}

size_t SampleHoldCard::tabAt(visage::Point position) const {
	if (folded()) {
		return kTabCount;
	}
	for (size_t t = 0; t < kTabCount; ++t) {
		if (tabBounds(t).contains(position)) {
			return t;
		}
	}
	return kTabCount;
}

bool SampleHoldCard::tabOn(size_t tab) const {
	return tab < kTabCount && host_.fieldAt(device_, fieldOf(tab, sh::Param::Enabled)) >= 0.5;
}

void SampleHoldCard::toggleTabOn(size_t tab) {
	if (tab >= kTabCount) {
		return;
	}
	// ONE CONTROL, ONE UNDO STEP — the same road a box's edit takes.
	DeviceControl& on = controlOfTab(tab, at(sh::Param::Enabled));
	on.setValue(on.value() >= 0.5 ? 0.0 : 1.0);
	redraw();
}

void SampleHoldCard::writeViewTab() {
	// A VIEW SETTING, NOT AN EDIT: no undo step, and written only when it differs.
	if (static_cast<size_t>(std::lround(host_.fieldAt(device_, sh::kViewTabField))) != tab_) {
		backplane::ControlGestureScope gesture(host_, device_);
		gesture.setField(sh::kViewTabField, static_cast<float>(tab_));
	}
}

void SampleHoldCard::selectTab(size_t tab) {
	if (tab >= kTabCount || tab == tab_) {
		return;
	}
	tab_ = tab;
	writeViewTab();
	// THE SCOPE FOLLOWS THE TAB: another hold's staircase, so the old tab's history is not its own.
	count_ = 0;
	head_ = 0;
	haveOrdinal_ = false;
	layoutControls();
	redraw();
}

void SampleHoldCard::showTabOutputs() {
	// THE CARD SHOWS THE PAIR OF EVERY TAB THAT IS ON (and tab 1's always): `SourceDesc::cardHidden` keeps the others off the
	// card until a hand turns them on. View state only.
	uint64_t mask = 0b11;
	for (size_t t = 1; t < kTabCount; ++t) {
		if (tabOn(t)) {
			mask |= uint64_t { 0b11 } << (2 * t);
		}
	}
	if (host_.outputsShown(device_) != mask) {
		host_.setOutputsShown(device_, mask);
	}
}

void SampleHoldCard::drawTabs(visage::Canvas& canvas) {
	for (size_t t = 0; t < kTabCount; ++t) {
		const visage::Bounds b = tabBounds(t);
		const bool selected = t == tab_;
		drawTabChip(canvas, b, std::string {}, selected);
		const bool on = tabOn(t);
		const visage::Bounds sq = tabOnBounds(t);
		canvas.setColor(selected ? kSurface : (on ? kAccent : kDisabled));
		if (on) {
			canvas.roundedRectangle(sq.x(), sq.y(), sq.width(), sq.height(), 2.0f);
		}
		else {
			canvas.roundedRectangleBorder(sq.x(), sq.y(), sq.width(), sq.height(), 2.0f, 1.0f);
		}
		canvas.setColor(selected ? kSurface : (on ? kMuted : kDisabled));
		drawTracked(canvas, std::to_string(t + 1), font(kKnobLabelSize), kKnobLabelTracking, sq.right() + 5.0f,
		            b.y(), b.height());
	}
}

size_t SampleHoldCard::visibleSetWidgetsForTest() const {
	size_t n = 0;
	for (const TabSet& set : sets_) {
		for (const auto& box : set.boxes) {
			n += box->isVisible() ? 1u : 0u;
		}
		for (const auto& chip : set.chips) {
			n += chip->isVisible() ? 1u : 0u;
		}
		n += set.trigPad->isVisible() ? 1u : 0u;
	}
	return n;
}

std::vector<size_t> SampleHoldCard::addressedFieldsForTest() const {
	// EVERY TAB'S SET IS REACHED BY ITS TAB — the selected tab's reach, offset to each — and the strip writes the view field.
	std::vector<size_t> out;
	const std::vector<sh::Param> one = addressedParamsForTest();
	for (size_t t = 0; t < kTabCount; ++t) {
		for (const sh::Param p : one) {
			out.push_back(fieldOf(t, p));
		}
		out.push_back(fieldOf(t, sh::Param::Enabled)); // the tab's On square
	}
	out.push_back(sh::kViewTabField);
	return out;
}

bool SampleHoldCard::headSourceFocused() const {
	for (size_t out = 0; out < std::size(sh::kOutputs); ++out) {
		if (mod_.source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool SampleHoldCard::headArmed() const {
	return headSourceFocused() && mod_.armed();
}

} // namespace feathers::ui
