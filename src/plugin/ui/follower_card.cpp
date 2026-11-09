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

#include "plugin/ui/follower_card.h"

#include "plugin/ui/edit_history.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace feathers::ui {

namespace {

namespace fl = feathers::mod::follower;

[[nodiscard]] constexpr size_t at(fl::Param p) {
	return static_cast<size_t>(p);
}

/// **THE FLIP, AS ONE UNDO STEP** — the Mode (and the Detector it moved), then every route the flip deleted. A record
/// over values and the host, capturing no widget (`edit_history.h`). Undo writes the fields back first, so each route
/// is admitted again by the gate it was made under; redo writes them forward, which deletes the routes again.
class FollowerModeEdit final : public EditRecord {
public:
	FollowerModeEdit(backplane::DeviceHost& host, DeviceInstanceId id, std::vector<DeviceFieldChange> changes,
	                 std::vector<backplane::Route> routes)
	    : host_(host), device_(id), changes_(std::move(changes)), routes_(std::move(routes)) {}

	void undo() const override {
		{
			backplane::ControlGestureScope gesture(host_, device_);
			for (const DeviceFieldChange& c : changes_) {
				gesture.setField(c.control, c.before);
			}
		}
		(void)host_.takePrunedRoutes(); // a flip back prunes nothing this step made; whatever it reports is not ours
		for (const backplane::Route& r : routes_) {
			host_.setRouteAmount(r.source, r.destination, r.amount);
			if (host_.supportsRouteTransfers()) {
				(void)host_.setRouteTransfer(r.source, r.destination, r.transfer);
			}
		}
	}
	void redo() const override {
		backplane::ControlGestureScope gesture(host_, device_);
		for (const DeviceFieldChange& c : changes_) {
			gesture.setField(c.control, c.after);
		}
		(void)host_.takePrunedRoutes();
	}
	const char* verb() const override { return "Mode"; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	std::vector<DeviceFieldChange> changes_;
	std::vector<backplane::Route> routes_;
};

/// ONE ROUTE IN WORDS, for the tray: "Follower Env → Filter Cutoff".
std::string routeWords(const backplane::DeviceHost& host, const backplane::Route& r) {
	std::string from = "a host source";
	if (r.source.isDevice() && !r.source.isOuter()) {
		const device::Descriptor* d = host.descriptorOf(r.source.device);
		from = std::string(d != nullptr ? d->traits.name : "Device") + ' '
		       + host.outputLabel(r.source.device, r.source.index);
	}
	return from + " \xE2\x86\x92 " + host.destinationName(r.destination);
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE MODE'S ROAD                                                                                 //
//------------------------------------------------------------------------------------------------//

FollowerModeControl::FollowerModeControl(backplane::DeviceHost& host, DeviceInstanceId device, ModState* mod,
                                         EditHistory* history, Notifier* notifier)
    : DeviceControl(host, device, fl::kDescriptor, at(fl::Param::Mode), mod, history), host_(host), notifier_(notifier) {}

void FollowerModeControl::beginGesture() {
	modeBefore_ = static_cast<float>(value());
	const size_t det = at(fl::Param::Detector);
	detectorBefore_ = det < host_.fieldsPresentAt(device()) ? host_.fieldAt(device(), det)
	                                                         : static_cast<float>(fl::kParamDescs[det].def);
	pruned_.clear();
	(void)host_.takePrunedRoutes(); // what an earlier write left is not this gesture's
	DeviceControl::beginGesture();
}

void FollowerModeControl::setValue(double value) {
	DeviceControl::setValue(value);
	for (backplane::Route& r : host_.takePrunedRoutes()) {
		pruned_.push_back(r);
	}
	// SIGNED IS CONTROL'S: a flip to Audio from it takes Peak, in the same step.
	const auto mode = static_cast<fl::Mode>(std::lround(this->value()));
	const size_t det = at(fl::Param::Detector);
	const float detector = det < host_.fieldsPresentAt(device()) ? host_.fieldAt(device(), det)
	                                                              : static_cast<float>(fl::kParamDescs[det].def);
	if (mode == fl::Mode::Audio && std::lround(detector) == static_cast<long>(fl::Detector::Signed)) {
		host_.setField(device(), det, static_cast<float>(fl::Detector::Peak));
	}
}

void FollowerModeControl::endGesture() {
	endHostGesture();
	const auto after = static_cast<float>(value());
	if (after == modeBefore_) {
		return;
	}
	std::vector<DeviceFieldChange> changes { { at(fl::Param::Mode), modeBefore_, after } };
	const size_t det = at(fl::Param::Detector);
	const float detectorAfter = det < host_.fieldsPresentAt(device()) ? host_.fieldAt(device(), det) : detectorBefore_;
	if (detectorAfter != detectorBefore_) {
		changes.push_back({ det, detectorBefore_, detectorAfter });
	}
	if (EditHistory* history = recordsInto(); history != nullptr) {
		history->record(std::make_unique<FollowerModeEdit>(host_, device(), std::move(changes), pruned_));
	}
	// THE TRAY: the mode, and every route it cost by name — the first in full, the rest counted.
	lastSaid_ = std::string("Mode ") + fl::kModeNames[std::lround(after) != 0 ? 1 : 0];
	if (!pruned_.empty()) {
		lastSaid_ += " \xE2\x80\x94 removed " + routeWords(host_, pruned_.front());
		if (pruned_.size() > 1) {
			lastSaid_ += " and " + std::to_string(pruned_.size() - 1) + (pruned_.size() == 2 ? " other route" : " other routes");
		}
		lastSaid_ += " (undo restores)";
	}
	if (notifier_ != nullptr) {
		notifier_->say(lastSaid_);
	}
}

//------------------------------------------------------------------------------------------------//
// THE CARD                                                                                        //
//------------------------------------------------------------------------------------------------//

FollowerCard::FollowerCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier, ModState* mod, EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	controls_.resize(fl::kDescriptor.params.size());
	auto modeControl = std::make_unique<FollowerModeControl>(host_, device_, mod_, history_, notifier_);
	modeControl_ = modeControl.get();
	controls_[at(fl::Param::Mode)] = std::move(modeControl);
	// THE MODE SAYS ITSELF, with the routes it cost — so the segmented control is handed no notifier.
	mode_ = std::make_unique<SegmentedControl>(*modeControl_, registry, fl::kModeNames, nullptr);
	addChild(mode_.get());
	poly_ = std::make_unique<SwitchChip>(control(at(fl::Param::Poly)), "Poly");
	addChild(poly_.get());
	detectorControl_ = std::make_unique<SegmentedControl>(control(at(fl::Param::Detector)), registry, fl::kDetectorNames,
	                                                      notifier_);
	addChild(detectorControl_.get());
	detectorAudio_ = std::make_unique<SegmentedControl>(control(at(fl::Param::Detector)), registry,
	                                                    fl::kAudioDetectorNames, notifier_);
	addChild(detectorAudio_.get());
	plot_ = std::make_unique<FollowerPlot>(host_, device_, fl::kDescriptor, at(fl::Param::Threshold));
	plot_->setModulation(mod_, history_);
	addChild(plot_.get());
	input_ = std::make_unique<Knob>(control(at(fl::Param::Input)), "Input", notifier_);
	attack_ = std::make_unique<Knob>(control(at(fl::Param::Attack)), "Attack", notifier_);
	release_ = std::make_unique<Knob>(control(at(fl::Param::Release)), "Release", notifier_);
	gain_ = std::make_unique<Knob>(control(at(fl::Param::Gain)), "Gain", notifier_);
	floor_ = std::make_unique<Knob>(control(at(fl::Param::Floor)), "Floor", notifier_);
	for (Knob* knob : { input_.get(), attack_.get(), release_.get(), gain_.get(), floor_.get() }) {
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

FollowerCard::~FollowerCard() = default;

DeviceControl& FollowerCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, fl::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

device::Modulatable* FollowerCard::enableModulatable() {
	return &control(at(fl::Param::Enabled));
}

void FollowerCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* knob : { input_.get(), attack_.get(), release_.get(), gain_.get(), floor_.get() }) {
		knob->setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
}

float FollowerCard::fieldOf(fl::Param p) const {
	const size_t c = at(p);
	return c < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, c) : static_cast<float>(fl::kParamDescs[c].def);
}

bool FollowerCard::underAudio() const {
	return std::lround(fieldOf(fl::Param::Mode)) == static_cast<long>(fl::Mode::Audio);
}

bool FollowerCard::underPoly() const {
	return underAudio() && fieldOf(fl::Param::Poly) >= 0.5f;
}

bool FollowerCard::underSigned() const {
	// WHAT THE CORE RUNS (`Settings::running`): Signed under Audio runs as Peak.
	return !underAudio() && std::lround(fieldOf(fl::Param::Detector)) == static_cast<long>(fl::Detector::Signed);
}

std::string FollowerCard::hears() const {
	if (underAudio()) {
		return underPoly() ? "hears its own voice, here in the chain" : "hears the audio here in the chain";
	}
	return "follows its Input: route any source into it";
}

std::vector<size_t> FollowerCard::addressedParamsForTest() const {
	std::vector<size_t> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(road->control());
		}
	};
	add(mode_->control());
	add(&poly_->control());
	add(detector().control());
	for (const Knob* knob : { input_.get(), attack_.get(), release_.get(), gain_.get(), floor_.get() }) {
		add(knob->control());
	}
	add(&plot_->thresholdControl());
	return out;
}

float FollowerCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float FollowerCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

void FollowerCard::resized() {
	layoutControls();
}

void FollowerCard::foldedChanged() {
	layoutControls();
}

void FollowerCard::layoutControls() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const bool open = !folded();
	audioLaid_ = underAudio();
	polyLaid_ = underPoly();
	signedLaid_ = underSigned();

	// THE MODE ROW: Mode at its share on the left, Poly at its own width on the right.
	float y = bodyTopEdge() + kGap;
	const float modeWidth = std::max(0.0f, inner * kModeShare);
	mode_->setBounds(left, y, modeWidth, kTabsHeight);
	mode_->setVisible(open);
	const float polyWidth = std::min(56.0f, std::max(0.0f, inner - modeWidth - kGap));
	poly_->setBounds(left + inner - polyWidth, y + 0.5f * (kTabsHeight - SwitchChip::kHeight), polyWidth,
	                 SwitchChip::kHeight);
	poly_->setVisible(open);
	y += kTabsHeight + kGap;

	// THE DETECTOR ROW: the mode's own menu across the card.
	for (SegmentedControl* d : { detectorControl_.get(), detectorAudio_.get() }) {
		d->setBounds(left, y, inner, kTabsHeight);
	}
	detectorControl_->setVisible(open && !audioLaid_);
	detectorAudio_->setVisible(open && audioLaid_);
	y += kTabsHeight + kGap + kHearsHeight + kGap;

	// THE PLOT, with Input's knob at its left — greyed in Audio, where it is not what the follower hears.
	input_->setBounds(left, y + 0.5f * (kPlotHeight - controlsHeight()), kInputColumnWidth, controlsHeight());
	input_->setVisible(open);
	input_->setActive(!audioLaid_);
	const float plotLeft = left + kInputColumnWidth + kGap;
	plot_->setBounds(plotLeft, y, std::max(0.0f, left + inner - plotLeft), kPlotHeight);
	plot_->setBipolar(signedLaid_);
	y += kPlotHeight + kGap;

	// THE DETECTOR'S TIME, THEN ITS LEVEL — the two times take the wider cells, because their labels are the longer
	// words: PHOTOGRAPHED at equal quarters, "RELEASE" was elided at the strip's width.
	constexpr float kShares[] = { 0.27f, 0.29f, 0.22f, 0.22f };
	Knob* row[] = { attack_.get(), release_.get(), gain_.get(), floor_.get() };
	const float cells = std::max(0.0f, inner - 3.0f * kRowGap);
	float x = left;
	for (size_t i = 0; i < std::size(row); ++i) {
		const float w = cells * kShares[i];
		row[i]->setBounds(x, y, w, controlsHeight());
		row[i]->setVisible(open);
		x += w + kRowGap;
	}
	// A FLOOR ON A BIPOLAR SIGNAL HAS NO ZERO TO LEAVE: greyed under Signed (the core does not apply it).
	floor_->setActive(!signedLaid_);

	plot_->setVisible(open);
}

bool FollowerCard::updateContent() {
	bool moved = false;
	if (underAudio() != audioLaid_ || underPoly() != polyLaid_ || underSigned() != signedLaid_) {
		layoutControls();
		redraw();
		moved = true;
	}
	if (!folded() && plot_->update()) {
		moved = true;
	}
	return moved;
}

void FollowerCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const float y = bodyTopEdge() + kGap + kTabsHeight + kGap + kTabsHeight + kGap;
	canvas.setColor(kMuted);
	canvas.text(hears(), font(kKnobReadoutSize), visage::Font::kLeft, left, y, inner, kHearsHeight);
}

bool FollowerCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < fl::kDescriptor.outputs.size(); ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool FollowerCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
