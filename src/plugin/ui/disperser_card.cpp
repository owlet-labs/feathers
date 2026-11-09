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

#include "plugin/ui/disperser_card.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace feathers::ui {

namespace dp = fx::disperser;

DisperserCard::DisperserCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                             Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry; // the device-control road carries no clap_id to watch (`CompCard`'s note)
	controls_.resize(static_cast<size_t>(dp::Param::kCount));

	plot_ = std::make_unique<DisperserPlot>(host, id, false);
	addChild(plot_.get());
	keyTrack_ = std::make_unique<SwitchChip>(control(dp::Param::KeyTrack), "Key");
	addChild(keyTrack_.get());
	freq_ = std::make_unique<ValueBox>(control(dp::Param::Freq), "Freq");
	freq_->setCompact(true);
	addChild(freq_.get());
	offset_ = std::make_unique<ValueBox>(control(dp::Param::Offset), "Offset");
	offset_->setCompact(true);
	addChild(offset_.get());
	stages_ = std::make_unique<ValueBox>(control(dp::Param::Stages), "Stages");
	stages_->setCompact(true);
	addChild(stages_.get());
	gamma_ = std::make_unique<GammaBox>(control(dp::Param::Gamma), "Gamma");
	addChild(gamma_.get());
	// TOPO AND TWIST: THE HOUSE'S MODE CELLS — the selector over the mode, a box per follower
	// (`ModeValueCell`, as `GenericExpanded` seats one from the descriptor's `modeCell`).
	const auto modeCell = [this](dp::Param mode, const char* caption) {
		const device::ParamDesc& d = dp::kParamDescs[static_cast<size_t>(mode)];
		std::vector<ZoneSelect::ZoneName> names;
		for (const char* n : d.names) {
			names.push_back({ n == nullptr ? "" : n, {} });
		}
		auto cell = std::make_unique<ModeValueCell>(control(mode), caption, std::move(names),
		                                            [this](size_t param) { return relevant(param); });
		for (size_t j = 0; j < std::size(dp::kParamDescs); ++j) {
			if (dp::kParamDescs[j].gateControl == static_cast<int16_t>(mode)) {
				cell->addFollower(j, std::make_unique<ValueBox>(control(static_cast<dp::Param>(j)), ""));
			}
		}
		cell->setFaceDiameter(kModeFace);
		addChild(cell.get());
		return cell;
	};
	topo_ = modeCell(dp::Param::TopoMode, "Topo");
	twist_ = modeCell(dp::Param::TwistMode, "Twist");
	// THE PILLS — the Shaper's extras (`ShaperCard`'s constructor), one per derived quantity, in the card's order.
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

	// THE HEAD'S OCCUPANTS — `CompCard`'s constructor.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Disperser on" : "Disperser off");
			}
		}
	};
}

DeviceControl& DisperserCard::control(dp::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, dp::kDescriptor, i);
	}
	return *controls_[i];
}

device::Modulatable* DisperserCard::enableModulatable() { return &control(dp::Param::Enabled); }

bool DisperserCard::headEnabled() const { return control(dp::Param::Enabled).value() >= 0.5; }

void DisperserCard::setModulation(ModState* mod, EditHistory* history) {
	// EVERY ADAPTER, including the plot's own — its axis is a control too.
	for (size_t i = 0; i < static_cast<size_t>(dp::Param::kCount); ++i) {
		control(static_cast<dp::Param>(i)).setModulation(mod, history);
	}
	plot_->setModulation(mod, history);
}

float DisperserCard::modeRowHeight() { return Knob::columnHeight(kModeFace); }

float DisperserCard::openHeight() {
	const float pillRows = static_cast<float>((dp::kPillCount + kPillsPerRow - 1) / kPillsPerRow);
	return kSlotHeadHeight + 4.0f + kPlotHeight + kRowGap + kControlRowHeight + kRowGap + modeRowHeight() + kRowGap
	       + pillRows * kPillHeight + (pillRows - 1.0f) * kPillGap + kPad;
}

visage::Bounds DisperserCard::pillBounds(size_t i) const {
	if (i >= dp::kPillCount) {
		return {};
	}
	// UNDER THE MODE CELLS, from the same terms `openHeight` sums. FITTED TO THE BODY, the width a ceiling rather
	// than a size — the Shaper's `extraBounds`, and its reasons (a fixed width hung the fifth pill off a 236px card).
	const float top = kSlotHeadHeight + 4.0f + kPlotHeight + kRowGap + kControlRowHeight + kRowGap + modeRowHeight()
	                  + kRowGap;
	const auto perRow = static_cast<float>(kPillsPerRow);
	const float usable = std::max(bodyWidth() - 2.0f * kPad - (perRow - 1.0f) * kPillGap, perRow);
	const float cell = std::min(kPillWidth, usable / perRow);
	const auto row = static_cast<float>(i / kPillsPerRow);
	const auto column = static_cast<float>(i % kPillsPerRow);
	return { bodyLeft() + kPad + column * (cell + kPillGap), top + row * (kPillHeight + kPillGap), cell, kPillHeight };
}

float DisperserCard::laneHeightAt(float) const { return folded() ? foldedHeight() : openHeight(); }

float DisperserCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

bool DisperserCard::tracking() const {
	return host_.fieldAt(device_, static_cast<size_t>(dp::Param::KeyTrack)) >= 0.5f;
}

void DisperserCard::resized() { layOut(); }

void DisperserCard::layOut() {
	laidOutTracking_ = tracking() ? 1 : 0;
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	float y = kSlotHeadHeight + 4.0f;

	// THE PLOT — the whole width. `setBounds` UNCONDITIONALLY (CLAUDE.md's DPI rule).
	plot_->setBounds(left, y, width, kPlotHeight);
	plot_->setVisible(open);
	y += kPlotHeight + kRowGap;

	// THE CONTROL ROW, grouped by what it changes: the centre (Key Track and its box), the count, gamma.
	// **EACH BOX IS SIZED BY WHAT IT MUST PRINT, THEN THE SURPLUS IS SHARED.** A compact box draws its
	// caption left and its number right over one row, clipping neither, so a box narrower than the two
	// together prints them on top of each other. Twice PHOTOGRAPHED: Stages taking 55% of the rest ran
	// "Gamma" into its value, and an even split still left Gamma's caption under "0.00". The widest
	// numbers are stated rather than read, because a width taken from the value showing now fits until
	// the value moves: Offset's "-6.50" and Freq's "8.2k" (the centre is one width in both Key states, so
	// the row does not reshuffle on the switch), Stages' "32", and Gamma's "-99.75" — its range is
	// unbounded, and two digits is what a drag reaches.
	const auto need = [](const char* caption, const char* number) {
		const std::string c(caption);
		const std::string n(number);
		return font(kKnobLabelSize).stringWidth(std::u32string(c.begin(), c.end())) + kCaptionGap
		       + font(kKnobReadoutSize).stringWidth(std::u32string(n.begin(), n.end()))
		       + 2.0f * ValueBox::kCompactPadX;
	};
	const float chipW = 32.0f;
	const float gap = 5.0f;
	float centreW = std::max(need("Offset", "-6.50"), need("Freq", "8.2k"));
	float stagesW = need("Stages", "32");
	float gammaW = need("Gamma", "-99.75");
	const float surplus = (width - chipW - 3.0f * gap - centreW - stagesW - gammaW) / 3.0f;
	centreW = std::max(0.0f, centreW + surplus);
	stagesW = std::max(0.0f, stagesW + surplus);
	gammaW = std::max(0.0f, gammaW + surplus);
	float x = left;
	keyTrack_->setBounds(x, y, chipW, SwitchChip::kHeight);
	keyTrack_->setVisible(open);
	x += chipW + gap;
	const float boxY = y + (kControlRowHeight - ValueBox::kCompactHeight) * 0.5f;
	freq_->setBounds(x, boxY, centreW, ValueBox::kCompactHeight);
	offset_->setBounds(x, boxY, centreW, ValueBox::kCompactHeight);
	freq_->setVisible(open && laidOutTracking_ == 0);
	offset_->setVisible(open && laidOutTracking_ == 1);
	x += centreW + gap;
	stages_->setBounds(x, boxY, stagesW, ValueBox::kCompactHeight);
	stages_->setVisible(open);
	x += stagesW + gap;
	gamma_->setBounds(x, boxY, std::max(0.0f, gammaW), ValueBox::kCompactHeight);
	gamma_->setVisible(open);
	y += kControlRowHeight + kRowGap;

	// THE TWO MODE CELLS, SIDE BY SIDE — a knob column each, as the Shaper seats its coordinate; the widest mode
	// name ("Parallel") fits half the card (`ModeValueCell::minimumWidth`, asserted).
	const float half = (width - gap) * 0.5f;
	topo_->setBounds(left, y, half, modeRowHeight());
	twist_->setBounds(left + half + gap, y, half, modeRowHeight());
	topo_->setVisible(open);
	twist_->setVisible(open);
	(void)topo_->refresh();
	(void)twist_->refresh();
	// AND THE PILLS.
	for (size_t i = 0; i < pills_.size(); ++i) {
		pills_[i]->setBounds(pillBounds(i));
		pills_[i]->setVisible(open);
	}
}

void DisperserCard::foldedChanged() { layOut(); }

bool DisperserCard::updateContent() {
	bool changed = false;
	// KEY TRACK MOVED (by automation, undo, a preset, the chip): the other centre box shows.
	if ((tracking() ? 1 : 0) != laidOutTracking_) {
		layOut();
		changed = true;
	}
	if (!folded() && plot_->update()) {
		changed = true;
	}
	// A MODE MOVED (a hand, automation, undo): the live follower's box shows — `ModeValueCell::refresh`.
	if (!folded() && (topo_->refresh() || twist_->refresh())) {
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void DisperserCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

bool DisperserCard::relevant(size_t param) const {
	if (param >= std::size(dp::kParamDescs)) {
		return false;
	}
	const device::ParamDesc& p = dp::kParamDescs[param];
	return p.gateControl < 0 || device::relevantFor(p, host_.fieldAt(device_, static_cast<size_t>(p.gateControl)));
}

void DisperserCard::drawContent(visage::Canvas&) {}

std::vector<dp::Param> DisperserCard::addressedParamsForTest() const {
	// **READ OFF THE WIDGET TREE, NOT WRITTEN DOWN.** A literal list here kept the reachability case green
	// with a widget deleted (a review found it). Every value box and switch chip the card holds — hidden
	// ones included, since the Key chip reveals them — answers with the control it draws, and the head's
	// square is Enabled. The plot's axis writes Freq or Offset too, but each has its own box, so a count
	// that leaned on the plot would stay whole with a box deleted.
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
			add(cell->select().control()); // the mode; its followers are the value boxes under it
		}
		// A PILL answers for its amount and its power — the pill at index i is pill i of `dp::Pill`.
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
	if (hasEnable()) {
		out.push_back(dp::Param::Enabled);
	}
	return out;
}

} // namespace feathers::ui
