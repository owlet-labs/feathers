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

#include "plugin/ui/phaser_expanded.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace feathers::ui {

namespace ph = fx::phaser;

namespace {

/// ONE UNIT OF ROTATE PER FULL DRAG — the shaper's `kShaperFieldDragSpan`, for the shaper's reason (a knob over the
/// declared ±8.4 million window is an unusably coarse dial).
constexpr double kRotateDragSpan = 1.0;

} // namespace

PhaserExpanded::PhaserExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : dhost_(host), device_(id), notifier_(notifier) {
	controls_.resize(ph::kParamCount);
	mode_ = std::make_unique<SegmentedControl>(control(ph::Param::Mode), registry, ph::kModeNames, notifier_);
	addChild(mode_.get());
	plot_ = std::make_unique<PhaserPlot>(host, id, true);
	addChild(plot_.get());
	const auto box = [this](ph::Param p, const char* caption) {
		auto b = std::make_unique<ValueBox>(control(p), caption);
		// THE DEVICE'S OWN READOUT (`kTextOps`: the LFO's rate under 1 Hz, Delay's milliseconds, Damping's "open") —
		// `PhaserCard`'s note.
		DeviceControl* adapter = &control(p);
		b->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
		addChild(b.get());
		boxes_.emplace_back(p, std::move(b));
	};
	// THE CARD'S CONTROLS, under the card's captions, then the ones the card has no room for.
	box(ph::Param::CenterHz, "Center");
	box(ph::Param::DelayMs, "Delay");
	box(ph::Param::Stages, "Stages");
	box(ph::Param::Feedback, "Feedback");
	box(ph::Param::Mix, "Mix");
	// WHILE A NOTE IS TRACKED, THE OFFSET FROM IT, in semitones — the card's readout (`phaserOffsetText`).
	for (const ph::Mode m : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		const ph::Param p = m == ph::Mode::Phaser ? ph::Param::CenterHz : ph::Param::DelayMs;
		DeviceControl* adapter = &control(p);
		boxForTest(p)->setReadoutFormatter([this, adapter, m](double at) {
			return phaserNoteTracked(dhost_, device_) ? phaserOffsetText(m, at) : adapter->textAt(at);
		});
	}
	box(ph::Param::RateHz, "Rate");
	box(ph::Param::Sync, "Sync");
	box(ph::Param::EnvAttack, "Attack");
	box(ph::Param::EnvRelease, "Release");
	box(ph::Param::EnvSens, "Sens");
	box(ph::Param::Position, "Position");
	box(ph::Param::Stereo, "Stereo");
	box(ph::Param::Drive, "Drive");
	box(ph::Param::Damping, "Damp");
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
	// THE EXPLORE PILLS, one per group in the groups' order (the header has the gates).
	static constexpr const char* kXCaptions[ph::phase_space::kGroupCount] = { "Spread", "Stagger", "Shape",   "Colour",
		                                                                      "Bias",   "Skew",    "Rate skew", "Fb tilt",
		                                                                      "Pairing", "Curve" };
	for (size_t g = 0; g < xPills_.size(); ++g) {
		const auto group = static_cast<ph::phase_space::Group>(g);
		xPills_[g] = pill(ph::pillKnobParam(group), ph::pillPowerParam(group), kXCaptions[g]);
		xActive_[g] = true;
	}
	(void)refreshLfoCaption();
	// **THE PAD, WIRED AS THE SHAPER'S IS** (`shaper_expanded.cpp`): the folded travel written by index past the
	// declared params, the row read by index. A child frame, so it takes its own presses.
	pad_ = std::make_unique<PhaserPad>(
	    [this](size_t index, float value) { dhost_.setField(device_, ph::kTravelFirst + index, value); },
	    [this](size_t field) { return dhost_.fieldAt(device_, field); }, control(ph::Param::Rotate));
	addChild(pad_.get());
	// ONE DRAG OF THE PAD IS ONE UNDO STEP (backlog JB), as the shaper's.
	pad_->onHoldBegan = [this] { padGesture_.begin(dhost_, device_, ph::kTravelFirst, PhaserPad::kPhaseCount); };
	pad_->onHoldEnded = [this] { (void)padGesture_.end(dhost_, history_, "Pad"); };
	// ROTATE, INSIDE THE TERRAIN BOX — made after the pad so it draws over the map it turns.
	rotate_ = std::make_unique<GammaBox>(control(ph::Param::Rotate), "Rotate");
	rotate_->setDragSpan(kRotateDragSpan);
	addChild(rotate_.get());
}

DeviceControl& PhaserExpanded::control(ph::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(dhost_, device_, ph::kDescriptor, i);
	}
	return *controls_[i];
}

void PhaserExpanded::modulationBound(ModState* mod, EditHistory* history) {
	history_ = history;
	for (size_t i = 0; i < ph::kParamCount; ++i) {
		control(static_cast<ph::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
}

std::string PhaserExpanded::title() const { return ph::kTraits.name; }

ValueBox* PhaserExpanded::boxForTest(ph::Param p) const {
	for (const auto& [param, box] : boxes_) {
		if (param == p) {
			return box.get();
		}
	}
	return nullptr;
}

float PhaserExpanded::rowsHeight() {
	// Mode above the plot; under it the centre row, Sweep's row, the two sources' rows, and the output row.
	return kSwitchHeight + kRowGap + kGap + 5.0f * ValueBox::kHeight + 4.0f * kRowGap;
}

visage::Point PhaserExpanded::minimumSize() const {
	const float left = 2.0f * kPad + rowsHeight() + kMinPlotHeight;
	// THE SIDE COLUMN: the pad, its caption, and the pills' two columns of five under it.
	const float pills = static_cast<float>(kXPillRows) * kPillHeight + static_cast<float>(kXPillRows - 1) * kXPillGap;
	const float right = 2.0f * kPad + kSideWidth + 20.0f + pills;
	return { 720.0f, std::max(left, right) };
}

visage::Bounds PhaserExpanded::padBounds() const {
	// A SQUARE OF THE SIDE COLUMN'S WIDTH, at the top — what the mock draws.
	const float x = width() - kPad - kSideWidth;
	// THE PILLS' BLOCK IS UNDER IT (caption, then two columns of five), so a short window shrinks the pad, not the pills.
	const float pills = static_cast<float>(kXPillRows) * kPillHeight + static_cast<float>(kXPillRows - 1) * kXPillGap;
	const float side = std::min(kSideWidth, std::max(0.0f, height() - 2.0f * kPad - 20.0f - pills));
	return { x, kPad, kSideWidth, side };
}

ph::Mode PhaserExpanded::modeNow() const {
	return std::lround(dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::Mode))) == static_cast<long>(ph::Mode::Flanger)
	           ? ph::Mode::Flanger
	           : ph::Mode::Phaser;
}

bool PhaserExpanded::gateOpen(ph::Param p) const {
	const device::ParamDesc& d = ph::kParamDescs[static_cast<size_t>(p)];
	return d.gateControl < 0
	       || device::relevantFor(d, dhost_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

void PhaserExpanded::resized() { layOut(); }

void PhaserExpanded::layOut() {
	const ph::Mode mode = modeNow();
	laidOutMode_ = static_cast<int>(mode);
	const bool phaser = mode == ph::Mode::Phaser;
	// EVERY `setBounds` UNCONDITIONAL (CLAUDE.md's DPI rule).
	const float left = kPad;
	const float colW = std::max(0.0f, width() - 2.0f * kPad - kSideWidth - kGap);
	float y = kPad;
	mode_->setBounds(left, y, colW, kSwitchHeight);
	y += kSwitchHeight + kRowGap;
	const float plotH = std::max(0.0f, height() - 2.0f * kPad - rowsHeight());
	plot_->setBounds(left, y, colW, plotH);
	y += plotH + kGap;

	// A ROW OF FOUR EQUAL CELLS — the card's grid, so a box sits in the same column it does there; a widget may span.
	const float cell = std::max(0.0f, (colW - 3.0f * kCellGap) / 4.0f);
	const auto cellX = [&](size_t column) { return left + static_cast<float>(column) * (cell + kCellGap); };
	const auto spanW = [&](size_t columns) { return static_cast<float>(columns) * cell + static_cast<float>(columns - 1) * kCellGap; };
	const auto put = [&](ph::Param p, size_t column, float top) {
		ValueBox* b = boxForTest(p);
		b->setBounds(cellX(column), top, cell, ValueBox::kHeight);
		b->setVisible(true);
	};

	// THE CENTRE ROW: the mode's centre box, Stages (Phaser only), Feedback, Mix — the card's row, re-flowed the same way.
	ValueBox* centre = boxForTest(phaser ? ph::Param::CenterHz : ph::Param::DelayMs);
	ValueBox* other = boxForTest(phaser ? ph::Param::DelayMs : ph::Param::CenterHz);
	ValueBox* stages = boxForTest(ph::Param::Stages);
	if (phaser) {
		put(ph::Param::CenterHz, 0, y);
		put(ph::Param::Stages, 1, y);
		put(ph::Param::Feedback, 2, y);
		put(ph::Param::Mix, 3, y);
	}
	else {
		// THREE OVER THE FOUR COLUMNS, as the card re-flows its row rather than leaving a hole.
		const float third = std::max(0.0f, (colW - 2.0f * kCellGap) / 3.0f);
		const ph::Param three[] = { ph::Param::DelayMs, ph::Param::Feedback, ph::Param::Mix };
		for (size_t i = 0; i < 3; ++i) {
			ValueBox* b = boxForTest(three[i]);
			b->setBounds(left + static_cast<float>(i) * (third + kCellGap), y, third, ValueBox::kHeight);
			b->setVisible(true);
		}
	}
	// The hidden boxes keep a size, so a widget walk that measures them reads a real one.
	other->setVisible(false);
	other->setBounds(centre->bounds());
	if (!phaser) {
		stages->setVisible(false);
		stages->setBounds(centre->bounds());
	}
	y += ValueBox::kHeight + kRowGap;

	// SWEEP, KEY, AND TZ IN THE FLANGER — a box's height, the switches centred on it.
	const float switchTop = y + (ValueBox::kHeight - kSwitchHeight) * 0.5f;
	sweep_->setBounds(left, switchTop, spanW(2), kSwitchHeight);
	keyTrack_->setBounds(cellX(2), switchTop, kKeyChipWidth, SwitchChip::kHeight);
	throughZero_->setBounds(cellX(3), switchTop, kTzChipWidth, SwitchChip::kHeight);
	throughZero_->setVisible(!phaser);
	y += ValueBox::kHeight + kRowGap;

	// THE TWO SOURCES, each pill beside what shapes it.
	const float pillTop = (ValueBox::kHeight - kPillHeight) * 0.5f;
	lfo_->setBounds(left, y + pillTop, spanW(2), kPillHeight);
	put(ph::Param::RateHz, 2, y);
	put(ph::Param::Sync, 3, y);
	y += ValueBox::kHeight + kRowGap;
	env_->setBounds(left, y + pillTop, cell, kPillHeight);
	put(ph::Param::EnvAttack, 1, y);
	put(ph::Param::EnvRelease, 2, y);
	put(ph::Param::EnvSens, 3, y);
	y += ValueBox::kHeight + kRowGap;

	put(ph::Param::Position, 0, y);
	put(ph::Param::Stereo, 1, y);
	put(ph::Param::Drive, 2, y);
	put(ph::Param::Damping, 3, y);

	const visage::Bounds pad = padBounds();
	pad_->setBounds(pad);
	constexpr float kInset = 6.0f;
	rotate_->setBounds(pad.right() - kRotateWidth - kInset, pad.y() + kInset, kRotateWidth, rotate_->naturalHeight());
	// THE EXPLORE PILLS: under the pad's caption, two columns of five, in the groups' order down the left then the right.
	const float pillTop0 = pad.bottom() + 20.0f;
	const float pillW = (pad.width() - kXPillGap) * 0.5f;
	for (size_t g = 0; g < xPills_.size(); ++g) {
		const size_t column = g / kXPillRows;
		const size_t row = g % kXPillRows;
		xPills_[g]->setBounds(pad.x() + static_cast<float>(column) * (pillW + kXPillGap),
		                      pillTop0 + static_cast<float>(row) * (kPillHeight + kXPillGap), pillW, kPillHeight);
	}
	(void)applyGates();
}

bool PhaserExpanded::applyGates() {
	bool changed = false;
	const bool lfoOn = dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::LfoOn)) >= 0.5f;
	for (const auto& [p, box] : boxes_) {
		bool on = gateOpen(p);
		// RATE: its descriptor gate (Sync = Free) AND the LFO's power, which its one gate cannot also say.
		if (p == ph::Param::RateHz) {
			on = on && lfoOn;
		}
		if (box->enabled() != on) {
			box->setEnabled(on);
			changed = true;
		}
	}
	// A TRACKED NOTE CHANGES WHAT THE CENTRE BOXES PRINT (the offset from it), not whether they take a hand.
	if (const bool tracked = phaserNoteTracked(dhost_, device_); tracked != shownTracked_) {
		shownTracked_ = tracked;
		boxForTest(ph::Param::CenterHz)->redraw();
		boxForTest(ph::Param::DelayMs)->redraw();
		changed = true;
	}
	// THE EXPLORE PILLS (the header lists each decision): the descriptor's gate where it has one, Colour while Damping
	// is running, Bias with some Drive.
	for (size_t g = 0; g < xPills_.size(); ++g) {
		const auto group = static_cast<ph::phase_space::Group>(g);
		bool live = gateOpen(ph::pillKnobParam(group));
		if (group == ph::phase_space::Group::Colour) {
			live = live && !ph::dampingOpen(dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::Damping)));
		}
		else if (group == ph::phase_space::Group::Bias) {
			live = live && dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::Drive)) > 0.0f;
		}
		else if (group == ph::phase_space::Group::RateSkew) {
			// THE LFO ON, OR A BARBERPOLE: ψ_R's rate moves R's windows with the LFO off too.
			const bool barber = ph::isBarberpole(static_cast<ph::SweepShape>(
			    std::clamp<long>(std::lround(dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::Sweep))), 0, 2)));
			live = live
			       && (dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::LfoOn)) >= 0.5f || barber);
		}
		if (live != xActive_[g]) {
			xActive_[g] = live;
			xPills_[g]->setActive(live);
			changed = true;
		}
	}
	// SWEEP HAS NO GATE (by design): the barberpole differs from Normal even with the LFO off (`kParamDescs`).
	return changed;
}

bool PhaserExpanded::refreshLfoCaption() {
	// THE CARD'S: what the LFO runs at, rebuilt only when Rate or Sync moves.
	const float rate = dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::RateHz));
	const float sync = dhost_.fieldAt(device_, static_cast<size_t>(ph::Param::Sync));
	if (rate == captionRate_ && sync == captionSync_) {
		return false;
	}
	captionRate_ = rate;
	captionSync_ = sync;
	const bool synced = ph::isSynced(static_cast<uint32_t>(std::max(0L, std::lround(sync))));
	lfo_->setReadout(synced ? control(ph::Param::Sync).text() : control(ph::Param::RateHz).text());
	return true;
}

bool PhaserExpanded::updateContent() {
	bool changed = false;
	// THE MODE MOVED: the centre box, Stages and TZ re-flow — the card's rule.
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
	// THE PLOT ASKS FOR ITS OWN FRAME — the card's rule: a moving LFO repaints the plot, not the window.
	if (plot_->update()) {
		changed = true;
	}
	return changed;
}

void PhaserExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width(), height());
	// UNDER THE PAD, what it is.
	const visage::Bounds pad = padBounds();
	canvas.setColor(kMuted);
	// WHAT THE TERRAIN SAYS (`PhaserPadPolicy::paint`) — not "home is the centre": the centre is where the sound IS.
	canvas.text("explore — dark is the textbook sound, bright is far from it", font(kKnobReadoutSize), visage::Font::kLeft,
	            pad.x(), pad.bottom() + 4.0f, pad.width(), 11.0f);
}

std::vector<ph::Param> PhaserExpanded::addressedParamsForTest() const {
	// **READ OFF THE WIDGET TREE, NOT WRITTEN DOWN** — what each widget offers (`ControlFace`), hidden ones included
	// (the Mode reveals them). The pad writes the travel, which is extra state, not a param.
	std::vector<ph::Param> out;
	std::vector<ControlFace::Offered> offered;
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ControlFace*>(frame)) {
			offered.clear();
			face->offeredControls(offered);
			for (const ControlFace::Offered& o : offered) {
				for (size_t i = 0; i < controls_.size(); ++i) {
					const auto p = static_cast<ph::Param>(i);
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
