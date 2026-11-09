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

#include "plugin/ui/phaser_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace feathers::ui {

namespace ph = fx::phaser;

PhaserCard::PhaserCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(ph::kParamCount);

	// TOP TO BOTTOM, the mock's order — each child is added in the order it is drawn.
	mode_ = std::make_unique<SegmentedControl>(control(ph::Param::Mode), registry, ph::kModeNames, notifier_);
	addChild(mode_.get());
	plot_ = std::make_unique<PhaserPlot>(host, id, false);
	addChild(plot_.get());
	const auto box = [this](ph::Param p, const char* caption) {
		auto b = std::make_unique<ValueBox>(control(p), caption);
		// THE DEVICE'S OWN READOUT (`kTextOps`: Damping's "open", Delay's milliseconds), as `GenericCard` seats it — a
		// box prints the unit's format otherwise, and Damping read "20k" for a loop with no filter in it.
		DeviceControl* adapter = &control(p);
		b->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
		addChild(b.get());
		return b;
	};
	center_ = box(ph::Param::CenterHz, "Center");
	delay_ = box(ph::Param::DelayMs, "Delay");
	// WHILE A NOTE IS TRACKED, THE OFFSET FROM IT, in semitones (`phaserOffsetText`) — the knob's meaning then.
	for (const ph::Mode m : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		ValueBox* b = m == ph::Mode::Phaser ? center_.get() : delay_.get();
		DeviceControl* adapter = &control(m == ph::Mode::Phaser ? ph::Param::CenterHz : ph::Param::DelayMs);
		b->setReadoutFormatter([this, adapter, m](double at) {
			return noteTracked() ? phaserOffsetText(m, at) : adapter->textAt(at);
		});
	}
	stages_ = box(ph::Param::Stages, "Stages");
	feedback_ = box(ph::Param::Feedback, "Feedback");
	mix_ = box(ph::Param::Mix, "Mix");
	sweep_ = std::make_unique<SegmentedControl>(control(ph::Param::Sweep), registry, ph::kSweepNames, notifier_);
	addChild(sweep_.get());
	keyTrack_ = std::make_unique<SwitchChip>(control(ph::Param::KeyTrack), "Key");
	addChild(keyTrack_.get());
	throughZero_ = std::make_unique<SwitchChip>(control(ph::Param::ThroughZero), "TZ");
	addChild(throughZero_.get());
	const auto pill = [this](ph::Param amount, ph::Param power, const char* caption) {
		auto p = std::make_unique<FaderPill>(control(amount), control(power), caption);
		p->setAnnouncer([this](const std::string& line) {
			if (notifier_ != nullptr) {
				notifier_->say(line);
			}
		});
		addChild(p.get());
		return p;
	};
	lfo_ = pill(ph::Param::LfoDepth, ph::Param::LfoOn, "LFO");
	env_ = pill(ph::Param::EnvAmount, ph::Param::EnvOn, "ENV");
	position_ = box(ph::Param::Position, "Position");
	stereo_ = box(ph::Param::Stereo, "Stereo");
	drive_ = box(ph::Param::Drive, "Drive");
	damping_ = box(ph::Param::Damping, "Damp");
	(void)refreshLfoCaption();

	// THE HEAD'S OCCUPANTS — `DisperserCard`'s constructor.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Phaser on" : "Phaser off");
			}
		}
	};
}

DeviceControl& PhaserCard::control(ph::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, ph::kDescriptor, i);
	}
	return *controls_[i];
}

device::Modulatable* PhaserCard::enableModulatable() { return &control(ph::Param::Enabled); }

bool PhaserCard::headEnabled() const { return control(ph::Param::Enabled).value() >= 0.5; }

void PhaserCard::setModulation(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < ph::kParamCount; ++i) {
		control(static_cast<ph::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
}

ValueBox* PhaserCard::boxForTest(ph::Param p) const {
	for (ValueBox* b : { center_.get(), delay_.get(), stages_.get(), feedback_.get(), mix_.get(), position_.get(),
	                     stereo_.get(), drive_.get(), damping_.get() }) {
		if (&b->control() == static_cast<const device::Modulatable*>(&control(p))) {
			return b;
		}
	}
	return nullptr;
}

float PhaserCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kSwitchHeight + kRowGap + kPlotHeight + kRowGap + ValueBox::kHeight + kRowGap
	       + kSwitchHeight + kRowGap + kPillHeight + kRowGap + ValueBox::kHeight + kPad;
}

float PhaserCard::laneHeightAt(float) const { return folded() ? foldedHeight() : openHeight(); }

float PhaserCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

ph::Mode PhaserCard::modeNow() const {
	return std::lround(host_.fieldAt(device_, static_cast<size_t>(ph::Param::Mode))) == static_cast<long>(ph::Mode::Flanger)
	           ? ph::Mode::Flanger
	           : ph::Mode::Phaser;
}

bool PhaserCard::gateOpen(ph::Param p) const {
	const device::ParamDesc& d = ph::kParamDescs[static_cast<size_t>(p)];
	return d.gateControl < 0
	       || device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

bool PhaserCard::noteTracked() const { return phaserNoteTracked(host_, device_); }

void PhaserCard::resized() { layOut(); }

void PhaserCard::foldedChanged() { layOut(); }

void PhaserCard::layOut() {
	const ph::Mode mode = modeNow();
	laidOutMode_ = static_cast<int>(mode);
	const bool phaser = mode == ph::Mode::Phaser;
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	float y = kSlotHeadHeight + 4.0f;

	// EVERY `setBounds` UNCONDITIONAL — CLAUDE.md's DPI rule: a scale change moves nothing logical.
	mode_->setBounds(left, y, width, kSwitchHeight);
	mode_->setVisible(open);
	y += kSwitchHeight + kRowGap;

	plot_->setBounds(left, y, width, kPlotHeight);
	plot_->setVisible(open);
	y += kPlotHeight + kRowGap;

	// A ROW OF EQUAL CELLS OVER WHICHEVER BOXES THE MODE SHOWS — the row re-flows rather than leaving a hole.
	const auto row = [&](std::initializer_list<ValueBox*> cells, float top) {
		const auto n = static_cast<float>(cells.size());
		const float cell = std::max(0.0f, (width - (n - 1.0f) * kCellGap) / n);
		float x = left;
		for (ValueBox* b : cells) {
			b->setBounds(x, top, cell, ValueBox::kHeight);
			b->setVisible(open);
			x += cell + kCellGap;
		}
	};
	ValueBox* centre = phaser ? center_.get() : delay_.get();
	ValueBox* other = phaser ? delay_.get() : center_.get();
	other->setVisible(false);
	if (phaser) {
		row({ centre, stages_.get(), feedback_.get(), mix_.get() }, y);
	}
	else {
		stages_->setVisible(false);
		row({ centre, feedback_.get(), mix_.get() }, y);
	}
	// The hidden boxes keep a size, so a widget walk that measures them reads a real one.
	other->setBounds(centre->bounds());
	if (!phaser) {
		stages_->setBounds(centre->bounds());
	}
	y += ValueBox::kHeight + kRowGap;

	// SWEEP, KEY, AND TZ IN THE FLANGER.
	const float chips = kKeyChipWidth + kCellGap + (phaser ? 0.0f : kTzChipWidth + kCellGap);
	const float sweepW = std::max(0.0f, width - chips);
	sweep_->setBounds(left, y, sweepW, kSwitchHeight);
	sweep_->setVisible(open);
	keyTrack_->setBounds(left + sweepW + kCellGap, y, kKeyChipWidth, SwitchChip::kHeight);
	keyTrack_->setVisible(open);
	throughZero_->setBounds(left + width - kTzChipWidth, y, kTzChipWidth, SwitchChip::kHeight);
	throughZero_->setVisible(open && !phaser);
	y += kSwitchHeight + kRowGap;

	// THE TWO SOURCES.
	const float half = (width - kCellGap) * 0.5f;
	lfo_->setBounds(left, y, half, kPillHeight);
	env_->setBounds(left + half + kCellGap, y, half, kPillHeight);
	lfo_->setVisible(open);
	env_->setVisible(open);
	y += kPillHeight + kRowGap;

	row({ position_.get(), stereo_.get(), drive_.get(), damping_.get() }, y);
	(void)applyGates();
}

bool PhaserCard::applyGates() {
	bool changed = false;
	const auto set = [&](ValueBox* b, bool on) {
		if (b->enabled() != on) {
			b->setEnabled(on);
			changed = true;
		}
	};
	// THE CENTRE: its mode gate (which box shows). A tracked note does not close it — the knob is the offset from the
	// note then — but it changes what the box PRINTS, so a change of tracking redraws both.
	set(center_.get(), gateOpen(ph::Param::CenterHz));
	set(delay_.get(), gateOpen(ph::Param::DelayMs));
	if (const bool tracked = noteTracked(); tracked != shownTracked_) {
		shownTracked_ = tracked;
		center_->redraw();
		delay_->redraw();
		changed = true;
	}
	set(stages_.get(), gateOpen(ph::Param::Stages));
	set(stereo_.get(), gateOpen(ph::Param::Stereo));
	// EVERY OTHER BOX IS UNGATED (`gateControl` -1), read rather than assumed so a gate added later takes effect here.
	const std::pair<ValueBox*, ph::Param> ungated[] = {
		{ feedback_.get(), ph::Param::Feedback }, { mix_.get(), ph::Param::Mix },
		{ position_.get(), ph::Param::Position }, { drive_.get(), ph::Param::Drive },
		{ damping_.get(), ph::Param::Damping },
	};
	for (const auto& [b, p] : ungated) {
		set(b, gateOpen(p));
	}
	// SWEEP HAS NO GATE (by decision): the barberpole differs from Normal even with the LFO off (`kParamDescs`).
	return changed;
}

bool PhaserCard::refreshLfoCaption() {
	// WHAT THE LFO RUNS AT — the rate, or the division when synced. Rebuilt only when either moves, so a card at rest
	// formats nothing.
	const float rate = host_.fieldAt(device_, static_cast<size_t>(ph::Param::RateHz));
	const float sync = host_.fieldAt(device_, static_cast<size_t>(ph::Param::Sync));
	if (rate == captionRate_ && sync == captionSync_) {
		return false;
	}
	captionRate_ = rate;
	captionSync_ = sync;
	const bool synced = ph::isSynced(static_cast<uint32_t>(std::max(0L, std::lround(sync))));
	lfo_->setReadout(synced ? control(ph::Param::Sync).text() : control(ph::Param::RateHz).text());
	return true;
}

bool PhaserCard::updateContent() {
	bool changed = false;
	// THE MODE MOVED (a hand, automation, undo, a preset): the centre box, Stages and TZ re-flow.
	if (static_cast<int>(modeNow()) != laidOutMode_) {
		layOut();
		mode_->redraw();
		changed = true;
	}
	if (applyGates()) {
		changed = true;
	}
	if (refreshLfoCaption()) {
		changed = true;
	}
	if (changed) {
		redraw();
	}
	// THE PLOT ASKS FOR ITS OWN FRAME (`PhaserPlot::update`): a moving LFO is a repaint of the plot, not of the card.
	if (!folded() && plot_->update()) {
		changed = true;
	}
	return changed;
}

void PhaserCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

void PhaserCard::drawContent(visage::Canvas&) {}

std::vector<ph::Param> PhaserCard::addressedParamsForTest() const {
	// **READ OFF THE WIDGET TREE, NOT WRITTEN DOWN** — `DisperserCard`'s reason. Hidden boxes count (the Mode reveals
	// them); the plot's axis writes Center or Delay too, but each has its own box, so a count leaning on the plot
	// would stay whole with a box deleted.
	std::vector<ph::Param> out;
	const auto add = [&](const device::Modulatable* m) {
		for (size_t i = 0; i < controls_.size(); ++i) {
			if (m != nullptr && controls_[i] != nullptr && static_cast<const device::Modulatable*>(controls_[i].get()) == m) {
				const auto p = static_cast<ph::Param>(i);
				if (std::find(out.begin(), out.end(), p) == out.end()) {
					out.push_back(p);
				}
			}
		}
	};
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* b = dynamic_cast<const ValueBox*>(frame)) {
			add(&b->control());
		}
		if (const auto* chip = dynamic_cast<const SwitchChip*>(frame)) {
			add(&chip->control());
		}
		if (const auto* seg = dynamic_cast<const SegmentedControl*>(frame)) {
			add(seg->control());
		}
		if (frame == lfo_.get()) {
			add(&control(ph::Param::LfoDepth));
			add(&control(ph::Param::LfoOn));
		}
		if (frame == env_.get()) {
			add(&control(ph::Param::EnvAmount));
			add(&control(ph::Param::EnvOn));
		}
		for (const visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(this);
	if (hasEnable()) {
		out.push_back(ph::Param::Enabled);
	}
	return out;
}

} // namespace feathers::ui
