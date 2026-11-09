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

#include "plugin/ui/comp_card.h"

#include "fx/comp/comp_lanes.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/tooltip.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "plugin/ui/plot_grid.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace cp = fx::comp;

namespace {

/// THE KNOBS, IN FLOW ORDER. One table, read by `layOut` AND by `addressedParamsForTest`. The order is
/// chosen so that each mode's live subset reads right when the hidden ones are skipped: Compress gets
/// Drive Threshold Ratio Knee Attack Release Makeup Mix Link, Limit gets Drive Ceiling Release Link. **DRIVE OPENS BOTH
/// (backlog MI)**: it is the gain in front of either gain computer, so it comes first, where the signal enters. **LINK
/// CLOSES BOTH (backlog LQ)**: it is live in both modes and it is about how the detectors listen, so it sits after each
/// mode's own controls rather than among them — and in Compress it fills the grid's last cell.
///
/// **COMPANDER (backlog QG)** gets Drive Ref Ratio Range Attack Release Makeup Mix Link: its own Ratio and Range sit where
/// Compress's Ratio and Knee do, so the three modes read in the same places.
constexpr std::array<cp::Param, 12> kKnobs {
	cp::Param::Drive,    cp::Param::Threshold, cp::Param::Ratio,   cp::Param::CmpRatio, cp::Param::Knee,
	cp::Param::CmpRange, cp::Param::Attack,    cp::Param::Ceiling, cp::Param::Release,  cp::Param::Makeup,
	cp::Param::Mix,      cp::Param::StereoLink,
};

/// THE CARD'S CAPTIONS, NOT THE DESCRIPTOR'S — the head already says "Compressor", so "Comp" in front
/// of every knob is width spent on nothing (`busutil_card.cpp` has the long form).
constexpr std::array<const char*, 12> kKnobCaptions {
	"Drive", "Thresh", "Ratio", "Ratio", "Knee", "Range", "Attack", "Ceiling", "Release", "Makeup", "Mix", "Link",
};
static_assert(kKnobs.size() == kKnobCaptions.size());
/// THRESHOLD IS THE COMPANDER'S REF — the level it leaves alone, not one it acts above — so it says so in that mode.
constexpr const char* kRefCaption = "Ref";
/// THE TOPOLOGY IN THE CARD'S WORDS (backlog QG): four names in a third of the room "Compander" needed, so the paired two
/// say their ORDER — C+E is compress, the lane, expand; E+C the other way round. The tooltip and the window say it in full.
constexpr std::array<const char*, 4> kTopologyCardNames { "Comp", "Exp", "C+E", "E+C" };
static_assert(kTopologyCardNames.size() == std::size(cp::kTopologyNames));

} // namespace

/// THE LANE CHIP — the Delay's (`DelayCard::LaneChip`): not a control; it opens the window, and says so.
class CompCard::LaneChip final : public visage::Frame, public TooltipSource {
public:
	std::string text;
	std::function<void()> onPress;
	const char* tooltipAt(visage::Point) const override { return kLaneChipTip; }
	void offeredControls(std::vector<Offered>&) const override {}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(withAlpha(kSurface, 0.85f));
		canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 3.0f);
		canvas.setColor(kMuted);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	}
	void mouseDown(const visage::MouseEvent&) override {
		if (onPress) {
			onPress();
		}
	}
};

CompCard::~CompCard() = default;

CompCard::CompCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                   Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(cp::Param::kCount));
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), kKnobCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// THE REGISTRY IS HANDED ON, not watched: these ride the device-control road, which carries no
	// clap_id to watch (`SegmentedControl`'s second constructor says so).
	mode_ = std::make_unique<SegmentedControl>(control(cp::Param::Mode), registry, cp::kModeNames, notifier_);
	addChild(mode_.get());
	detector_ = std::make_unique<SegmentedControl>(control(cp::Param::Detector), registry, cp::kDetectorNames, notifier_);
	addChild(detector_.get());
	detect_ = std::make_unique<SegmentedControl>(control(cp::Param::Detect), registry, cp::kDetectNames,
	                                             notifier_);
	addChild(detect_.get());
	// COMPACT, AND ITS MENU OPENS ITSELF — nine named entries is past `choiceMenuAvailable`'s three.
	scHpf_ = std::make_unique<ValueBox>(control(cp::Param::ScHpf), "SC HPF");
	scHpf_->setCompact(true);
	addChild(scHpf_.get());
	// L/R OR M/S (backlog LQ) — two states, so a chip; it prints the live choice, since neither is "On".
	stereo_ = std::make_unique<SwitchChip>(control(cp::Param::StereoMode), "Stereo");
	addChild(stereo_.get());
	// THE COMPANDER'S TOPOLOGY (backlog QG) — every choice on the face, in the card's short words (`kTopologyCardNames`);
	// the window prints the full ones.
	topology_ = std::make_unique<SegmentedControl>(control(cp::Param::Topology), registry, kTopologyCardNames, notifier_);
	addChild(topology_.get());
	// THE LANE'S OPENER (backlog QG): the Delay's `FB FX` chip — it counts the lane's devices and opens the window.
	laneChip_ = std::make_unique<LaneChip>();
	laneChip_->onPress = [this] {
		if (onExpand) {
			onExpand();
		}
	};
	laneChip_->text = laneChipText();
	installTooltipHover(*laneChip_, *laneChip_);
	addChild(laneChip_.get());

	// THE HEAD'S OCCUPANTS — `BusUtilCard`'s constructor, minus the polarity pair this card does not
	// have. `=` on the press list only: the base's reorder lives on drag and release.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Compressor on" : "Compressor off");
			}
		}
	};
}

DeviceControl& CompCard::control(cp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, cp::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* CompCard::enableModulatable() {
	return &control(cp::Param::Enabled);
}

bool CompCard::headEnabled() const {
	return control(cp::Param::Enabled).value() >= 0.5;
}

void CompCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// THE KNOBS, WHICH DRAW THEIR OWN ARM GLOW — and EVERY ADAPTER, including the ones no knob holds.
	// `BusUtilCard::setModulation` has both halves of the reasoning.
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(cp::Param::kCount); ++i) {
		control(static_cast<cp::Param>(i)).setModulation(mod, history);
	}
}

float CompCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kSwitchHeight + kSwitchGap + kSwitchHeight + kSwitchGap
	       + static_cast<float>(kKnobRows) * (kCellHeight + kRowGap) + kSwitchHeight + kSwitchGap + kSwitchHeight + kSwitchGap
	       + kPlotHeight + kPad;
}

float CompCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float CompCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

visage::Bounds CompCard::headBounds() const {
	return ownFrame().head;
}

float CompCard::contentRight() const {
	return bodyLeft() + bodyWidth() - kPad - kMeterWidth - kMeterGap;
}

visage::Bounds CompCard::meterBounds() const {
	// UNDER THE MODE ROW (backlog QG): three modes need the card's whole width for their names, so the row runs over the
	// meter's column and the meter starts a row lower.
	const float top = kSlotHeadHeight + 4.0f + 2.0f * (kSwitchHeight + kSwitchGap);
	// THE CARD'S OWN HEIGHT when a host hands it less than its open height (the standalone rack), so the meter is not clipped.
	const float bottom = height() > 0.0f ? std::min(height(), openHeight()) : openHeight();
	return { contentRight() + kMeterGap, top, kMeterWidth, std::max(0.0f, bottom - kPad - top) };
}

void CompCard::resized() { layOut(); }

int CompCard::modeNow() const {
	return static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(cp::Param::Mode))));
}

bool CompCard::isLive(cp::Param p) const {
	// THE DESCRIPTOR DECIDES, the instance supplies the gates' values — and a gate's own gate is asked too (`liveIn`:
	// the Compander's controls nest, backlog QG).
	return cp::liveIn(p, [this](size_t i) { return host_.fieldAt(device_, i); });
}

void CompCard::layOut() {
	laidOutMode_ = modeNow();
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, contentRight() - left);

	// ROW 1 — THE MODE, every choice on the face, ACROSS THE CARD'S WHOLE WIDTH (backlog QG): three names, "Compander" the
	// longest, did not fit beside the stereo chip in the content's width, so the row runs over the meter's column (the
	// meter starts a row lower) and the chip moved to the Detector row's right end.
	float y = kSlotHeadHeight + 4.0f;
	mode_->setBounds(left, y, std::max(0.0f, bodyLeft() + bodyWidth() - kPad - left), kSwitchHeight);
	mode_->setVisible(open);
	y += kSwitchHeight + kSwitchGap;

	// ROW 1b — THE COMPANDER'S TOPOLOGY (backlog QG), all four on the face, the card's whole width; empty in the other
	// modes, so the card's height does not move with Mode.
	topology_->setBounds(left, y, std::max(0.0f, bodyLeft() + bodyWidth() - kPad - left), kSwitchHeight);
	topology_->setVisible(open && isLive(cp::Param::Topology));
	y += kSwitchHeight + kSwitchGap;

	// ROW 2 — THE LIVE KNOBS, CLOSED UP. A hidden knob takes no cell, so Limit's three sit at the left
	// of the first row rather than in the holes Compress's would leave. `setBounds` UNCONDITIONALLY, so a
	// scale change reaches every knob (CLAUDE.md's DPI rule).
	const float cell = width / static_cast<float>(kKnobsPerRow);
	size_t at = 0;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const bool live = isLive(kKnobs[i]);
		const size_t row = at / kKnobsPerRow;
		const size_t col = at % kKnobsPerRow;
		knobs_[i]->setBounds(left + cell * static_cast<float>(col), y + static_cast<float>(row) * (kCellHeight + kRowGap),
		                     cell, kCellHeight);
		knobs_[i]->setVisible(open && live);
		if (live) {
			++at;
		}
	}
	y += static_cast<float>(kKnobRows) * (kCellHeight + kRowGap);

	// ROW 3 — Detect (Compress only) and the sidechain high-pass (both modes), closed up the same way.
	const float half = (width - kRowGap) * 0.5f;
	const bool detectLive = isLive(cp::Param::Detect);
	detect_->setBounds(left, y, half, kSwitchHeight);
	detect_->setVisible(open && detectLive);
	const float boxLeft = detectLive ? left + half + kRowGap : left;
	scHpf_->setBounds(boxLeft, y + (kSwitchHeight - ValueBox::kCompactHeight) * 0.5f, half,
	                  ValueBox::kCompactHeight);
	scHpf_->setVisible(open);
	y += kSwitchHeight + kSwitchGap;

	// ROW 4 — THE DETECTOR (Compress only, backlog JO): Self or Side on the face, and beside it the line that says why Side is
	// compressing nothing. The card is as tall as Compress needs, so Limit leaves this row empty rather than moving the cards below.
	const bool detectorLive = isLive(cp::Param::Detector);
	detector_->setBounds(left, y, half, kSwitchHeight);
	detector_->setVisible(open && detectorLive);
	// THE STEREO CHIP (backlog LQ) at this row's right end, in every mode — a second choice about the whole device.
	stereo_->setBounds(left + width - kChipWidth, y + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kChipWidth,
	                   SwitchChip::kHeight);
	stereo_->setVisible(open);
	y += kSwitchHeight + kSwitchGap;

	// ROW 5 — THE TRANSFER PLOT (backlog QG), every mode, the content's full width. "sidechain silent" is printed inside
	// its top edge: the Detector row has the chip at its end now.
	plot_ = { left, y, width, kPlotHeight };
	sideNote_ = { left + 4.0f, y + 2.0f, std::max(0.0f, width - 8.0f), kSwitchHeight };
	// THE LANE CHIP (backlog QG): the plot's top-right corner, in Compander only — the Delay's `FB FX` chip.
	laneChip_->setBounds(plot_.right() - kLaneChipWidth - 3.0f, plot_.y() + 3.0f, kLaneChipWidth, kLaneChipHeight);
	laneChip_->setVisible(open && plotFits() && laidOutMode_ == static_cast<int>(cp::Mode::Compander));

	// THRESHOLD IS THE COMPANDER'S REF, and its caption says which (`setLabel` reaches the glass only).
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == cp::Param::Threshold) {
			knobs_[i]->setLabel(laidOutMode_ == static_cast<int>(cp::Mode::Compander) ? kRefCaption : kKnobCaptions[i]);
		}
	}
}

void CompCard::foldedChanged() {
	// A FOLDED CARD DRAWS NO BODY — and the laid-out visibility is the gate's AND the fold's, so one
	// function decides both rather than two that could disagree.
	layOut();
}

//------------------------------------------------------------------------------------------------//
// THE READINGS                                                                                    //
//------------------------------------------------------------------------------------------------//

bool CompCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(cp::Reading::kCount);
}

float CompCard::reading(cp::Reading r) const {
	// NOTHING PUBLISHED DRAWS NOTHING (`drawMeter` asks `hasReadings()` first); 0 is a linear silence
	// and no reduction, the honest nothing for all three if something draws anyway.
	return hasReadings() ? host_.readingAt(device_, static_cast<size_t>(r)) : 0.0f;
}

bool CompCard::sidechainSilent() const {
	return isLive(cp::Param::Detector)
	       && std::lround(host_.fieldAt(device_, static_cast<size_t>(cp::Param::Detector)))
	              == static_cast<long>(cp::Detector::Side)
	       && hasReadings() && reading(cp::Reading::SidechainSounding) < 0.5f;
}

CompCard::MeterFractions CompCard::meterFractions() const {
	const comp_plot::Meter m = meter();
	return { m.in, m.out, m.gr };
}

void CompCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE SLOT MENU IS DRAWN HERE, OUTSIDE THE BODY'S CLAMP** — `FxCard::drawHeadExtras` defaults to
	// nothing, and a card that does not override it has a working "..." press with nothing drawn to
	// press (the missing "..." header item). See `FilterCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

bool CompCard::updateContent() {
	bool changed = false;
	// THE MODE FIRST, AND WHETHER OR NOT ANYTHING IS PUBLISHING: a mode switched by automation, undo or
	// a preset must re-hide the knobs whether the device is running or not.
	if (modeNow() != laidOutMode_) {
		layOut();
		mode_->redraw();
		changed = true;
	}
	// **PUBLISHED TO UNPUBLISHED IS A CHANGE TOO** — the comparison below runs only while something is
	// published, so without this a meter that lost its data would go on showing its last bars instead of
	// "no signal".
	const bool has = hasReadings();
	if (has != drawnHadReadings_) {
		drawnHadReadings_ = has;
		changed = true;
	}
	if (has) {
		const float in = reading(cp::Reading::InPeak);
		const float out = reading(cp::Reading::OutPeak);
		const float gr = reading(cp::Reading::GainReduction);
		if (in != drawnIn_ || out != drawnOut_ || gr != drawnGr_) {
			drawnIn_ = in;
			drawnOut_ = out;
			drawnGr_ = gr;
			changed = true;
		}
	}
	// THE LANE'S COUNT, re-asked every tick (the surface is a contemporaneous handle, trap T9).
	if (std::string text = laneChipText(); text != laneChip_->text) {
		laneChip_->text = std::move(text);
		laneChip_->redraw();
		changed = true;
	}
	// THE TRANSFER PLOT (backlog QG) follows the knobs that shape it, whoever moved them.
	if (const auto curve = curveFields(); curve != drawnCurve_) {
		drawnCurve_ = curve;
		changed = true;
	}
	// THE "SIDECHAIN SILENT" LINE comes and goes with the reading and with the Detector control, whichever moved.
	if (const bool silent = sidechainSilent(); silent != drawnSilent_) {
		drawnSilent_ = silent;
		changed = true;
	}
	// **ASK FOR THE FRAME HERE — returning true is the report, not the delivery.** `FxLane::updateCards`
	// only counts; `busutil_card.cpp`'s `updateContent` records the meter that computed, compared and
	// never drew.
	if (changed) {
		redraw();
	}
	return changed;
}

//------------------------------------------------------------------------------------------------//
// DRAWING                                                                                         //
//------------------------------------------------------------------------------------------------//

comp_plot::Meter CompCard::meter() const {
	return comp_plot::meterOf(host_.readingCountAt(device_), [this](size_t i) { return host_.readingAt(device_, i); });
}

void CompCard::drawMeter(visage::Canvas& canvas) {
	// THE ONE METER ROAD (`comp_plot::meterOf` / `paintMeter`): the window draws the same bars from the same readings.
	comp_plot::paintMeter(canvas, meterBounds(), meter());
}

void CompCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawMeter(canvas);
	drawTransfer(canvas);
	if (sidechainSilent()) {
		// THE HONESTY LINE (backlog JO): Side with nothing on the sidechain compresses nothing, and says so, as the vocoder does.
		canvas.setColor(kMuted);
		canvas.text("sidechain silent", font(kKnobLabelSize), visage::Font::kLeft, sideNote_.x(), sideNote_.y(), sideNote_.width(),
		            sideNote_.height());
	}
}


//------------------------------------------------------------------------------------------------//
// THE TRANSFER PLOT (backlog QG)                                                                  //
//------------------------------------------------------------------------------------------------//

float CompCard::field(cp::Param p) const {
	return static_cast<float>(host_.fieldAt(device_, static_cast<size_t>(p)));
}

std::array<float, CompCard::kCurveFields> CompCard::curveFields() const {
	std::array<float, kCurveFields> out {};
	static constexpr std::array<cp::Param, kCurveFields> kFields { cp::Param::Mode,     cp::Param::Threshold, cp::Param::Ratio,
		                                                           cp::Param::Knee,     cp::Param::CmpRatio,  cp::Param::CmpRange,
		                                                           cp::Param::Drive,    cp::Param::Makeup,    cp::Param::Mix,
		                                                           cp::Param::Ceiling,  cp::Param::Topology,  cp::Param::CmpLink,
		                                                           cp::Param::CmpRatio2 };
	for (size_t i = 0; i < kFields.size(); ++i) {
		out[i] = field(kFields[i]);
	}
	return out;
}

size_t CompCard::laneDevices() const {
	const rack::InnerChain* lane = fx::comp::companderLaneOf(host_.containerSurfaceOf(device_));
	return lane != nullptr && lane->bridge() != nullptr ? lane->bridge()->chainLength() : 0;
}

std::string CompCard::laneChipText() const {
	const size_t n = laneDevices();
	return n == 0 ? std::string(kLaneChipEmpty) : std::string(kLaneChipPrefix) + std::to_string(n);
}

visage::Frame& CompCard::laneChipForTest() const {
	return *laneChip_;
}

comp_plot::Curves CompCard::curves() const {
	return comp_plot::curvesOf([this](size_t i) { return host_.fieldAt(device_, i); });
}

float CompCard::transferDb(float xDb) const {
	return curves().netDb(xDb);
}

bool CompCard::plotFits() const {
	// A HOST THAT HANDS THE CARD LESS THAN ITS OPEN HEIGHT (the standalone rack: 336 px) folds the plot away rather than
	// clipping it; the window has it, and the head's expand button opens the window.
	return height() + 0.5f >= openHeight();
}

void CompCard::drawTransfer(visage::Canvas& canvas) {
	if (!plotFits()) {
		return;
	}
	const bool has = hasReadings();
	comp_plot::paint(canvas, plot_, curves(), has, has ? reading(cp::Reading::InPeak) : 0.0f);
}

std::vector<cp::Param> CompCard::addressedParamsForTest() const {
	// EVERY CONTROL THE SURFACE REACHES, HIDDEN OR NOT — the Mode control reveals the rest, and
	// reachability is about the surface, not about one mode of it.
	std::vector<cp::Param> out(kKnobs.begin(), kKnobs.end());
	out.push_back(cp::Param::Mode);
	out.push_back(cp::Param::Detect);
	out.push_back(cp::Param::Detector);
	out.push_back(cp::Param::ScHpf);
	out.push_back(cp::Param::StereoMode);
	out.push_back(cp::Param::Topology);
	out.push_back(cp::Param::Enabled);
	return out;
}

Knob* CompCard::knobForTest(cp::Param p) const {
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == p) {
			return knobs_[i].get();
		}
	}
	return nullptr;
}

} // namespace feathers::ui
