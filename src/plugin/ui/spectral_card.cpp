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

#include "plugin/ui/spectral_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace sp = fx::spectral;

namespace {

/// THE KNOBS THE HANDLES DO NOT CARRY. Of the two filters' Slopes (F2), only the selected filter's shows (the tab is the window's,
/// shared with the card: SpectrumSource::selectedFilter), so the window always shows the one the card does.
constexpr std::array<sp::Param, 7> kKnobs { sp::Param::Mix, sp::Param::FilterSlope, sp::Param::Filter2Slope, sp::Param::CombPhase,
	                                        sp::Param::OddEvenBalance, sp::Param::PartialsCount, sp::Param::PartialsDepth };
constexpr std::array<const char*, 7> kKnobCaptions { "Mix", "Slope", "Slope 2", "Phase", "Balance", "Partials", "Depth" };
/// The fields that move a widget's visibility (and the filter tab, visibilityKey's last bit).
constexpr std::array<sp::Param, 8> kVisibilityFields { sp::Param::Poly, sp::Param::VocodeOn, sp::Param::FilterOn, sp::Param::Filter2On,
	                                                   sp::Param::CombOn, sp::Param::OddEvenOn, sp::Param::PartialsOn,
	                                                   sp::Param::Enabled };

} // namespace

SpectralCard::SpectralCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier), source_(host, registry, id) {
	// THE CARD AT A GLANCE (C1, the simplified card): the plot with its handles, the badge while the vocoder
	// listens to a silent sidechain, and the knobs the handles do not carry. The switches, Resolution, Key, Poly, Offset, the Carrier
	// and the Draw tool live in the window (which holds every control the card does, and more).
	badge_ = std::make_unique<SidechainBadge>();
	addChild(badge_.get());
	plot_ = std::make_unique<SpectrumPlot>(source_);
	addChild(plot_.get());
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(source_.deviceControl(kKnobs[i]), kKnobCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Spectral on" : "Spectral off");
			}
		}
	};
}

device::Modulatable* SpectralCard::enableModulatable() { return &source_.deviceControl(sp::Param::Enabled); }

bool SpectralCard::headEnabled() const { return on(sp::Param::Enabled); }

bool SpectralCard::on(sp::Param p) const {
	const auto i = static_cast<size_t>(p);
	const float v = i < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, i) : static_cast<float>(sp::kParamDescs[i].def);
	return v >= 0.5f;
}

bool SpectralCard::shown(sp::Param p) const {
	switch (p) {
	case sp::Param::FilterMode:
	case sp::Param::FilterSlope: return on(sp::Param::FilterOn) && source_.selectedFilter() == 0;
	case sp::Param::Filter2Mode:
	case sp::Param::Filter2Slope: return on(sp::Param::Filter2On) && source_.selectedFilter() == 1;
	case sp::Param::CombPhase: return on(sp::Param::CombOn);
	case sp::Param::OddEvenBalance: return on(sp::Param::OddEvenOn);
	case sp::Param::PartialsMode:
	case sp::Param::PartialsDepth: return on(sp::Param::PartialsOn);
	// THE COUNT IS NOT READ BY CENTERS (OI), which keeps its set's handful instead: hidden there, as the window hides it.
	case sp::Param::PartialsCount: return on(sp::Param::PartialsOn) && source_.settings().partialsMode != 2;
	case sp::Param::VocodeOn: return !on(sp::Param::Poly); // the bus's: the per-voice road hears no sidechain
	default: return true;
	}
}

uint32_t SpectralCard::visibilityKey() const {
	uint32_t key = 0;
	for (size_t i = 0; i < kVisibilityFields.size(); ++i) {
		key |= on(kVisibilityFields[i]) ? 1u << i : 0u;
	}
	return key | (source_.selectedFilter() == 1 ? 1u << kVisibilityFields.size() : 0u)
	       | (source_.settings().partialsMode == 2 ? 1u << (kVisibilityFields.size() + 1) : 0u); // Centers hides the count
}

void SpectralCard::setModulation(ModState* mod, EditHistory* history) {
	history_ = history;
	plot_->setHistory(history);
	source_.bindModulation(mod, history);
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
}

float SpectralCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kPlotHeight + kRowGap + kCellHeight + kPad;
}

float SpectralCard::laneHeightAt(float) const { return folded() ? foldedHeight() : openHeight(); }

float SpectralCard::laneExtent(float cross) const { return folded() ? kCardHeadHeight : FxCard::laneExtent(cross); }

void SpectralCard::resized() { layOut(); }

void SpectralCard::foldedChanged() { layOut(); }

void SpectralCard::layOut() {
	laidOutKey_ = visibilityKey();
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyLeft() + bodyWidth() - kPad - left);
	float y = kSlotHeadHeight + 4.0f;

	// THE BADGE'S LINE, while Vocode is on (and not hidden by Poly): "sidechain silent" while nothing sounds there. The plot gives
	// the line up, so the card's height never moves.
	const bool vocodeLine = on(sp::Param::VocodeOn) && shown(sp::Param::VocodeOn);
	badge_->setBounds(left, y, width, SwitchChip::kHeight);
	badge_->setVisible(open && vocodeLine);
	if (vocodeLine) {
		y += vocodeLineHeight();
	}

	// THE PLOT TAKES WHAT THE CARD IS GIVEN: kPlotHeight in a lane (laneHeightAt asks for it), less where a host hands the card a
	// fixed height (the rack's unit, backlog KQ), so every row below it stays inside the card.
	const float plotHeight = std::clamp(height() - (openHeight() - kPlotHeight) - (vocodeLine ? vocodeLineHeight() : 0.0f), kMinPlotHeight,
	                                    kPlotHeight);
	plot_->setBounds(left, y, width, plotHeight);
	plot_->setVisible(open);
	y += plotHeight + kRowGap;

	// THE KNOBS, packed: only the ones whose operator is on, a quarter of the width each until more than four are live.
	size_t liveKnobs = 0;
	for (const sp::Param p : kKnobs) {
		liveKnobs += shown(p) ? 1 : 0;
	}
	const float cell = width / static_cast<float>(std::max<size_t>(4, liveKnobs));
	size_t at = 0;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const bool live = shown(kKnobs[i]);
		knobs_[i]->setBounds(left + cell * static_cast<float>(at), y, cell, kCellHeight);
		knobs_[i]->setVisible(open && live);
		at += live ? 1 : 0;
	}
}

void SpectralCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

bool SpectralCard::updateContent() {
	bool changed = false;
	if (visibilityKey() != laidOutKey_) {
		layOut();
		changed = true;
	}
	if (!folded()) {
		const bool silent = on(sp::Param::VocodeOn) && shown(sp::Param::VocodeOn) && !source_.sidechainSounding();
		if (silent != badge_->silent()) {
			badge_->setSilent(silent);
			changed = true;
		}
		changed = plot_->sync(0, true, false) || changed; // no Draw tool on the card: it is the window's
		// SLOPE GREYS AT ITS FILTER'S OPEN END (Z7d; a Bell's is 0 dB): the filter is off there, so it changes nothing — the honesty rule.
		const bool open[2] = { source_.filterOpen(0, 0), source_.filterOpen(0, 1) };
		for (size_t i = 0; i < kKnobs.size(); ++i) {
			const bool active = !((kKnobs[i] == sp::Param::FilterSlope && open[0]) || (kKnobs[i] == sp::Param::Filter2Slope && open[1]));
			if (knobs_[i]->active() != active) {
				knobs_[i]->setActive(active);
				changed = true;
			}
		}
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void SpectralCard::drawContent(visage::Canvas&) {}

std::vector<sp::Param> SpectralCard::addressedParamsForTest() const {
	std::vector<sp::Param> out { sp::Param::Enabled };
	out.insert(out.end(), kKnobs.begin(), kKnobs.end());
	// THE PLOT'S HANDLES (the drawer's): the filter's corner, the comb's notch, the vowel, Tilt's grip.
	for (const sp::Param p : { sp::Param::FilterCutoff, sp::Param::FilterResonance, sp::Param::FilterGain, sp::Param::Filter2Cutoff,
	                           sp::Param::Filter2Resonance, sp::Param::Filter2Gain, sp::Param::CombSpacing, sp::Param::CombDepth,
	                           sp::Param::Vowel, sp::Param::VowelDepth, sp::Param::TiltAmount }) {
		out.push_back(p);
	}
	return out;
}

visage::Frame* SpectralCard::widgetForTest(sp::Param p) const {
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == p) {
			return knobs_[i].get();
		}
	}
	return nullptr;
}

std::vector<visage::Frame*> SpectralCard::widgetsForTest() const {
	std::vector<visage::Frame*> out { plot_.get(), badge_.get() };
	for (const auto& k : knobs_) {
		out.push_back(k.get());
	}
	return out;
}

} // namespace feathers::ui

namespace feathers::ui {

SidechainBadge::SidechainBadge() {
	// LAST, after every `=` (there are none) — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void SidechainBadge::setSilent(bool silent) {
	if (silent != silent_) {
		silent_ = silent;
		redraw();
	}
}

void SidechainBadge::draw(visage::Canvas& canvas) {
	if (!silent_) {
		return;
	}
	canvas.setColor(kMuted);
	canvas.text("sidechain silent", font(kKnobLabelSize), visage::Font::kLeft, 0.0f, 0.0f, width(), height());
}

const char* SidechainBadge::tooltipAt(visage::Point) const {
	return silent_ ? fx::spectral::kParamDescs[static_cast<size_t>(fx::spectral::Param::VocodeOn)].tooltip : nullptr;
}

} // namespace feathers::ui
