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

#include "plugin/ui/disperser_expanded.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace feathers::ui {

namespace dp = fx::disperser;

DisperserExpanded::DisperserExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                     Notifier* notifier)
    : dhost_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_.resize(static_cast<size_t>(dp::Param::kCount));
	plot_ = std::make_unique<DisperserPlot>(host, id, true);
	addChild(plot_.get());
	// TOPO AND TWIST: THE HOUSE'S MODE CELLS, as the card seats them (the standard zone control: a dropdown
	// and an infinite value box in one cell). The window's old name lists are gone: the standard
	// is the dropdown, which shows every name on demand before anything moves.
	const auto modeCell = [this](dp::Param mode, const char* caption) {
		const device::ParamDesc& d = dp::kParamDescs[static_cast<size_t>(mode)];
		std::vector<ZoneSelect::ZoneName> names;
		for (const char* n : d.names) {
			names.push_back({ n == nullptr ? "" : n, {} });
		}
		auto cell = std::make_unique<ModeValueCell>(control(mode), caption, std::move(names), [this](size_t param) {
			if (param >= std::size(dp::kParamDescs)) {
				return false;
			}
			const device::ParamDesc& p = dp::kParamDescs[param];
			return p.gateControl < 0
			       || device::relevantFor(p, dhost_.fieldAt(device_, static_cast<size_t>(p.gateControl)));
		});
		for (size_t j = 0; j < std::size(dp::kParamDescs); ++j) {
			if (dp::kParamDescs[j].gateControl == static_cast<int16_t>(mode)) {
				cell->addFollower(j, std::make_unique<ValueBox>(control(static_cast<dp::Param>(j)), ""));
			}
		}
		cell->setFaceDiameter(kCellFace);
		addChild(cell.get());
		return cell;
	};
	topo_ = modeCell(dp::Param::TopoMode, "Topo");
	twist_ = modeCell(dp::Param::TwistMode, "Twist");
	for (size_t i = 0; i < dp::kPillCount; ++i) {
		const auto pill = static_cast<dp::Pill>(i);
		auto widget = std::make_unique<FaderPill>(control(dp::pillAmountParam(pill)), control(dp::pillOnParam(pill)),
		                                          dp::kPillNames[i]);
		widget->setAnnouncer([this](const std::string& line) {
			if (notifier_ != nullptr) {
				notifier_->say(line);
			}
		});
		addChild(widget.get());
		pills_.push_back(std::move(widget));
	}
	gamma_ = std::make_unique<GammaBox>(control(dp::Param::Gamma), "Gamma");
	addChild(gamma_.get());
	stages_ = std::make_unique<ValueBox>(control(dp::Param::Stages), "Stages");
	stages_->setCompact(true);
	addChild(stages_.get());
	keyTrack_ = std::make_unique<SwitchChip>(control(dp::Param::KeyTrack), "Key Track");
	addChild(keyTrack_.get());
	freq_ = std::make_unique<ValueBox>(control(dp::Param::Freq), "Freq");
	freq_->setCompact(true);
	addChild(freq_.get());
	offset_ = std::make_unique<ValueBox>(control(dp::Param::Offset), "Offset");
	offset_->setCompact(true);
	addChild(offset_.get());
	// THE Q OFFSET (2026-10-03) — the plot handle's height, as a box too: typed, reset, and armed for a route here.
	qOffset_ = std::make_unique<ValueBox>(control(dp::Param::QOffset), "Q Offset");
	qOffset_->setCompact(true);
	addChild(qOffset_.get());
}

DeviceControl& DisperserExpanded::control(dp::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(dhost_, device_, dp::kDescriptor, i);
	}
	return *controls_[i];
}

void DisperserExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < static_cast<size_t>(dp::Param::kCount); ++i) {
		control(static_cast<dp::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
}

std::string DisperserExpanded::title() const { return dp::kTraits.name; }

visage::Point DisperserExpanded::minimumSize() const {
	// THE COLUMN: the two mode cells, the pills, then the three control rows.
	const float side = sideTopHeight() + 3.0f * (kControlRow + kGap) + 2.0f * kPad;
	return { 640.0f, std::max(360.0f, side) };
}

float DisperserExpanded::sideTopHeight() const {
	const float pillRows = static_cast<float>((dp::kPillCount + kPillsPerRow - 1) / kPillsPerRow);
	return Knob::columnHeight(kCellFace) + kGap + pillRows * kPillHeight + (pillRows - 1.0f) * kPillGap + kGap;
}

bool DisperserExpanded::tracking() const {
	return dhost_.fieldAt(device_, static_cast<size_t>(dp::Param::KeyTrack)) >= 0.5f;
}

visage::Bounds DisperserExpanded::stripBounds() const {
	const float left = kPad;
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - kSideWidth - kGap);
	return { left, height() - kPad - kStripHeight, plotW, kStripHeight };
}

void DisperserExpanded::resized() { layOut(); }

void DisperserExpanded::layOut() {
	laidOutTracking_ = tracking() ? 1 : 0;
	const visage::Bounds strip = stripBounds();
	plot_->setBounds(kPad, kPad, strip.width(), std::max(0.0f, strip.y() - kGap - kPad));

	const float x = width() - kPad - kSideWidth;
	// THE MODE CELLS AND THE PILLS FROM THE TOP, THE CONTROL ROWS ON THE STRIP'S FOOT — so a tall window's spare
	// room falls between them rather than under everything.
	float y = kPad;
	const float half = (kSideWidth - kGap) * 0.5f;
	topo_->setBounds(x, y, half, Knob::columnHeight(kCellFace));
	twist_->setBounds(x + half + kGap, y, half, Knob::columnHeight(kCellFace));
	(void)topo_->refresh();
	(void)twist_->refresh();
	y += Knob::columnHeight(kCellFace) + kGap;
	const auto perRow = static_cast<float>(kPillsPerRow);
	const float cell = (kSideWidth - (perRow - 1.0f) * kPillGap) / perRow;
	for (size_t i = 0; i < pills_.size(); ++i) {
		const auto row = static_cast<float>(i / kPillsPerRow);
		const auto column = static_cast<float>(i % kPillsPerRow);
		pills_[i]->setBounds(x + column * (cell + kPillGap), y + row * (kPillHeight + kPillGap), cell, kPillHeight);
	}
	y = std::max(kPad + sideTopHeight(), height() - kPad - 3.0f * kControlRow - 2.0f * kGap);
	gamma_->setBounds(x, y, half, ValueBox::kCompactHeight);
	stages_->setBounds(x + half + kGap, y, half, ValueBox::kCompactHeight);
	y += kControlRow + kGap;
	keyTrack_->setBounds(x, y, half, SwitchChip::kHeight);
	freq_->setBounds(x + half + kGap, y, half, ValueBox::kCompactHeight);
	offset_->setBounds(x + half + kGap, y, half, ValueBox::kCompactHeight);
	freq_->setVisible(laidOutTracking_ == 0);
	offset_->setVisible(laidOutTracking_ == 1);
	y += kControlRow + kGap;
	qOffset_->setBounds(x, y, half, ValueBox::kCompactHeight);
}

DisperserExpanded::Strip DisperserExpanded::stripNow() const {
	Strip s;
	if (dhost_.readingCountAt(device_) < static_cast<size_t>(dp::Reading::kCount)) {
		return s;
	}
	const auto at = [&](dp::Reading r) { return dhost_.readingAt(device_, static_cast<size_t>(r)); };
	s.live = true;
	s.punch = at(dp::Reading::PunchGain);
	s.chirp = at(dp::Reading::ChirpGain);
	s.chirp2F = at(dp::Reading::Chirp2FGain);
	s.loop = at(dp::Reading::LoopGain);
	s.echoMs = at(dp::Reading::EchoMs);
	return s;
}

bool DisperserExpanded::updateContent() {
	bool changed = false;
	if ((tracking() ? 1 : 0) != laidOutTracking_) {
		layOut();
		changed = true;
	}
	if (plot_->update()) {
		changed = true;
	}
	// A MODE MOVED: the live follower's box shows (`ModeValueCell::refresh`).
	if (topo_->refresh()) {
		changed = true;
	}
	if (twist_->refresh()) {
		changed = true;
	}
	const Strip now = stripNow();
	if (now.live != drawnStrip_.live || now.punch != drawnStrip_.punch || now.chirp != drawnStrip_.chirp
	    || now.chirp2F != drawnStrip_.chirp2F || now.loop != drawnStrip_.loop || now.echoMs != drawnStrip_.echoMs) {
		drawnStrip_ = now;
		++stripRepaints_;
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void DisperserExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, width(), height());

	// THE FEEDBACK STRIP — the delay road's gains this block, and the loop against the cap's ceiling.
	const visage::Bounds box = stripBounds();
	canvas.setColor(kSurface);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 5.0f, 1.0f);
	const Strip& s = drawnStrip_;
	canvas.setColor(kMuted);
	canvas.text("FEEDBACK", font(kKnobReadoutSize), visage::Font::kLeft, box.x() + 8.0f, box.y() + 3.0f, 90.0f, 11.0f);
	if (!s.live) {
		canvas.text("no signal yet", font(kKnobReadoutSize), visage::Font::kLeft, box.x() + 8.0f, box.y() + 20.0f,
		            160.0f, 11.0f);
		return;
	}
	struct Bar {
		const char* name;
		float value; ///< 0..1 of the bar
		uint32_t colour;
	};
	// THE WRITES ARE DOUBLED ON THE WAY IN (the firmware's), so a gain of 0.5 fills its bar.
	const Bar bars[] = {
		{ "Punch", std::clamp(s.punch * 2.0f, 0.0f, 1.0f), kAccent },
		{ "Chirp", std::clamp(s.chirp * 2.0f, 0.0f, 1.0f), kAccent },
		{ "2f", std::clamp(s.chirp2F * 2.0f, 0.0f, 1.0f), kAccent },
		{ "Loop", std::clamp(s.loop / 1.2f, 0.0f, 1.0f), s.loop > dp::kChirpLoopCeiling ? kModulation : kValue },
	};
	const float cellW = (box.width() - 110.0f) / 4.0f;
	const float barY = box.y() + 17.0f;
	for (size_t i = 0; i < 4; ++i) {
		const float x = box.x() + 8.0f + cellW * static_cast<float>(i);
		canvas.setColor(kMuted);
		canvas.text(bars[i].name, font(kKnobReadoutSize), visage::Font::kLeft, x, barY + 8.0f, cellW, 11.0f);
		const float barX = x + 34.0f;
		const float barW = std::max(0.0f, cellW - 44.0f);
		canvas.setColor(kLine);
		canvas.fill(barX, barY + 11.0f, barW, 5.0f);
		canvas.setColor(bars[i].colour);
		canvas.fill(barX, barY + 11.0f, barW * bars[i].value, 5.0f);
		if (i == 3) {
			// THE CAP'S CEILING, as a tick: the loop bar crossing it is the cap at work.
			canvas.setColor(kInk);
			canvas.fill(barX + barW * (dp::kChirpLoopCeiling / 1.2f), barY + 8.0f, 1.0f, 11.0f);
		}
	}
	char text[32];
	std::snprintf(text, sizeof(text), "echo %.0f ms", static_cast<double>(s.echoMs));
	canvas.setColor(kInk);
	canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight, box.right() - 100.0f, barY + 8.0f, 92.0f, 11.0f);
}

std::vector<dp::Param> DisperserExpanded::addressedParamsForTest() const {
	// THE CARD'S WALK (`DisperserCard::addressedParamsForTest`): every value box, switch chip, mode cell and pill
	// answers with the control it draws.
	std::vector<dp::Param> out;
	const auto add = [&](const device::Modulatable& m) {
		for (size_t i = 0; i < controls_.size(); ++i) {
			if (controls_[i] != nullptr && static_cast<const device::Modulatable*>(controls_[i].get()) == &m) {
				const auto p = static_cast<dp::Param>(i);
				if (std::find(out.begin(), out.end(), p) == out.end()) {
					out.push_back(p);
				}
			}
		}
	};
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* box = dynamic_cast<const ValueBox*>(frame)) {
			add(box->control());
		}
		if (const auto* chip = dynamic_cast<const SwitchChip*>(frame)) {
			add(chip->control());
		}
		if (const auto* cell = dynamic_cast<const ModeValueCell*>(frame)) {
			add(cell->select().control());
		}
		for (size_t i = 0; i < pills_.size(); ++i) {
			if (frame == pills_[i].get()) {
				add(control(dp::pillAmountParam(static_cast<dp::Pill>(i))));
				add(control(dp::pillOnParam(static_cast<dp::Pill>(i))));
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
