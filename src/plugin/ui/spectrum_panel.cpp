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

#include "plugin/ui/spectrum_panel.h"

#include "dsp/phase_profile.h"
#include "dsp/spectral_shaper.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/pill_stack.h"
#include "plugin/ui/device_control.h" // `modulationBound` binds a device's controls too (QK)
#include "plugin/ui/stored_edit.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace feathers::ui {

namespace {

/// Filter Mode's segments, short enough for the panel's head; the param's own labels are the long ones (the menu, the host).
constexpr std::array<const char* const, 5> kModeSegments { "LP", "HP", "BP", "Notch", "Bell" };
/// F2's tabs: which filter's controls the row shows.
constexpr std::array<const char* const, 2> kFilterTabNames { "Filter 1", "Filter 2" };
/// The Partials cell's dropdown lines, one per scheme (kSpectralPartialsModeNames' order).
constexpr std::array<const char* const, 3> kPartialsSchemeLines { "The N loudest partials of the sound",
	                                                              "The lowest N partials, from the fundamental up",
	                                                              "Only the fundamental and a few low harmonics — turn for another set" };
static_assert(kPartialsSchemeLines.size() == kSpectralPartialsModeNames.size());
/// A filter panel's switch is captioned "On": the tab beside it names the filter.
constexpr const char* kFilterChipLabel = "On";
constexpr const char* kRoutingChipLabel = "Parallel";
/// The head of the chipless panel of the vocoder's engine-wide analysis (shown only while some sound vocodes).
constexpr const char* kAnalysisHead = "VOCODE ANALYSIS";
static_assert(kModeSegments.size() == kSpectralFilterModeNames.size());

constexpr float kTabRounding = 9.0f;
constexpr float kPanelRounding = 8.0f;
constexpr float kButtonRounding = 5.0f;
constexpr float kTabTracking = 0.24f;

/// Each continuous control's caption in the row (the param's name is the long one: tooltips, the host).
const char* captionOf(SpectralControl id) {
	switch (id) {
	case SpectralControl::FilterCutoff: case SpectralControl::Filter2Cutoff: return "Cutoff";
	case SpectralControl::FilterResonance: case SpectralControl::Filter2Resonance: return "Res";
	case SpectralControl::FilterSlope: case SpectralControl::Filter2Slope: return "Slope";
	case SpectralControl::FilterKeyTrack: case SpectralControl::Filter2KeyTrack: return "Key";
	case SpectralControl::FilterGain: case SpectralControl::Filter2Gain: return "Gain";
	case SpectralControl::CombSpacing: return "Spacing";
	case SpectralControl::CombDepth: return "Depth";
	case SpectralControl::CombPhase: return "Phase";
	case SpectralControl::FormantShiftLow: return "Low";
	case SpectralControl::FormantShiftHigh: return "High";
	case SpectralControl::FormantShiftCrossover: return "X-over";
	case SpectralControl::TiltAmount: return "Amount";
	case SpectralControl::OddEvenBalance: return "Balance";
	case SpectralControl::FormantVowel: return "Vowel";
	case SpectralControl::FormantDepth: return "Depth";
	case SpectralControl::PartialsCount: return "Count";
	case SpectralControl::PartialsDepth: return "Depth";
	case SpectralControl::DampTime: return "Time";
	case SpectralControl::DampTilt: return "Tilt";
	case SpectralControl::StretchAmount: return "Amount";
	case SpectralControl::ShiftHz: return "Hz";
	case SpectralControl::VocodeDepth: return "Depth";
	case SpectralControl::VocodeFlatten: return "Flatten";
	case SpectralControl::VocodeGate: return "Gate";
	case SpectralControl::VocodeLow: return "Low";
	case SpectralControl::VocodeHigh: return "High";
	case SpectralControl::VocodeBandwidth: return "Width";
	default: return "";
	}
}

/// **FLATTEN AS A BUTTON OVER THE PLOT** (a dense window; the drawer draws it in the curve panel's head). One action, not a
/// control: every band of the selected sound's curve back to unity, one edit. Greyed (and deaf) while the curve is off.
} // namespace

class SpectrumFlattenButton final : public visage::Frame, public TooltipSource {
public:
	explicit SpectrumFlattenButton(std::function<void()> act) : act_(std::move(act)) {
		onMouseDown() = [this](const visage::MouseEvent&) {
			if (enabled_) {
				act_();
			}
		};
		// LAST, after every `=` above — see `installTooltipHover`.
		installTooltipHover(*this, *this);
	}
	void setEnabled(bool enabled) {
		if (enabled != enabled_) {
			enabled_ = enabled;
			redraw();
		}
	}
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }
	const char* tooltipAt(visage::Point) const override {
		return "Flatten: every band of the drawn curve back to 0 dB, as one edit.";
	}
	void offeredControls(std::vector<Offered>&) const override {} // an action on the curve, not a device control
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(withAlpha(kSurface, 0.85f));
		canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kButtonRounding);
		canvas.setColor(enabled_ ? kLine : withAlpha(kLine, 0.5f));
		canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), kButtonRounding, 1.0f);
		const visage::Font& control = font(kKnobLabelSize);
		const std::string caption = "FLATTEN";
		const float cw = trackedWidth(control, caption, kControlTracking);
		canvas.setColor(enabled_ ? kMuted : kDisabled);
		drawTracked(canvas, caption, control, kControlTracking, (width() - cw) * 0.5f, 3.0f, height() - 6.0f);
	}

private:
	std::function<void()> act_;
	bool enabled_ = true;
};

namespace {

/// A chip as wide as its caption (SwitchChip prints it plain, in the knob-label face) and some air.
float chipWidth(const char* label) {
	return trackedWidth(font(kKnobLabelSize), label, 0.0f) + 20.0f;
}

/// **THE FILTER TAB AS A CONTROL A SegmentedControl CAN DRAW** (F2): the source's selection, which is VIEW STATE — no param, no
/// destination, no undo step, never saved (the EQ's band selection is the precedent). `moved` re-lays the row at once.
class FilterTabControl final : public device::Modulatable {
public:
	FilterTabControl(SpectrumSource& source, std::function<void()> moved) : source_(source), moved_(std::move(moved)) {
		desc_.key = "spectral_filter_tab";
		desc_.name = "Filter";
		desc_.min = 0.0;
		desc_.max = 1.0;
		desc_.def = 0.0;
		desc_.stepped = true;
		desc_.unit = device::Unit::Choice;
		desc_.names = kFilterTabNames;
		desc_.tooltip = "Which filter's controls are shown: both are always heard, and both handles stay on the plot";
	}
	const device::ParamDesc& desc() const override { return desc_; }
	double value() const override { return static_cast<double>(source_.selectedFilter()); }
	void setValue(double value) override {
		source_.selectFilter(value >= 0.5 ? 1 : 0);
		if (moved_) {
			moved_();
		}
	}
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return kFilterTabNames[static_cast<size_t>(source_.selectedFilter())]; }

private:
	SpectrumSource& source_;
	std::function<void()> moved_;
	device::ParamDesc desc_ {};
};

/// THE TABS: a SegmentedControl that OFFERS NO CONTROL — the tab is where the eye is, not a device control, so the window ⊇
/// card guard does not count it (CLAUDE.md: the EQ's band selection is outside the guard, and so is this).
class FilterTabs final : public SegmentedControl {
public:
	using SegmentedControl::SegmentedControl;
	void offeredControls(std::vector<Offered>&) const override {}
};

} // namespace

// ---- StoredSwitch -----------------------------------------------------------------------------------------------

StoredSwitch::StoredSwitch(const char* key, const char* name, const char* tooltip, std::function<bool()> get,
                           std::function<void(bool)> set)
    : desc_ { key, name, 0.0, 1.0, 1.0, true, device::Unit::Choice, kEnableNames },
      get_(std::move(get)),
      set_(std::move(set)) {
	desc_.tooltip = tooltip;
}

namespace {

/// ONE FLIP OF A STORED SWITCH, as one undo step (the host's undo never sees stored state).
class StoredSwitchEdit final : public EditRecord {
public:
	StoredSwitchEdit(std::function<void(bool)> set, bool before, const char* verb)
	    : set_(std::move(set)), before_(before), verb_(verb) {}
	void undo() const override { set_(before_); }
	void redo() const override { set_(!before_); }
	const char* verb() const override { return verb_; }

private:
	std::function<void(bool)> set_;
	bool before_;
	const char* verb_;
};

} // namespace

void StoredSwitch::setValue(double value) {
	const bool before = get_();
	const bool after = value >= 0.5;
	set_(after);
	if (history_ != nullptr && after != before) {
		history_->record(std::make_unique<StoredSwitchEdit>(set_, before, desc_.name));
	}
}

// ---- SpectrumTab ------------------------------------------------------------------------------------------------

SpectrumTab::SpectrumTab(ParamBridge& bridge, std::function<void()> togglePanel)
    : bridge_(bridge),
      toggle_(std::move(togglePanel)),
      master_(kSpectralOnKey, "Spectral",
              "Spectral processing on or off. Off is the engine's own sound, and every setting is kept.",
              [&bridge] { return bridge.spectralOn(); },
              // ONE UNDO STEP PER FLIP (backlog JB) is StoredSwitch's own record, so the setter only sets: a second record
              // here would make each flip two steps, and an undo (which calls this) record a third.
              [&bridge](bool on) { bridge.setSpectralOn(on); }) {
	chip_ = std::make_unique<SwitchChip>(master_, "On");
	// NOT A PARAM CONTROL (backlog QK): the spectral master switch is stored state (`StoredSwitch`), not a destination.
	chip_->setShowsModulation(false);
	addChild(chip_.get());
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (labelBounds().contains(e.position) && toggle_) {
			toggle_();
		}
	};
}

SpectrumTab::~SpectrumTab() = default;

void SpectrumTab::resized() {
	const float chip = std::min(width() - 16.0f, 44.0f);
	chip_->setBounds((width() - chip) * 0.5f, kLabelHeight + 2.0f, chip, SwitchChip::kHeight);
}

void SpectrumTab::setOpen(bool open) {
	if (open != open_) {
		open_ = open;
		redraw();
	}
}

void SpectrumTab::sync(bool enabled) {
	const bool lit = bridge_.spectralOn();
	if (lit != lit_) {
		lit_ = lit;
		redraw();
	}
	chip_->setEnabled(enabled);
}

void SpectrumTab::draw(visage::Canvas& canvas) {
	// THE SKETCH'S TAB: a raised surface with an open bottom edge, the word lit in the accent while the master switch is on.
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height() + kTabRounding, kTabRounding);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height() + kTabRounding, kTabRounding, 1.0f);
	const visage::Font& face = font(kKnobLabelSize + 1.0f);
	const std::string word = "SPECTRUM";
	const float w = trackedWidth(face, word, kTabTracking);
	const float caret = 8.0f;
	const float x = (width() - w - caret) * 0.5f;
	canvas.setColor(lit_ ? kAccent : kMuted);
	drawTracked(canvas, word, face, kTabTracking, x, 4.0f, kLabelHeight - 6.0f);
	// THE CARET LIGHTS WHILE THE PANEL IS OPEN — told by the editor, which owns the panel (ExploreStage::setSpectrumOpen).
	canvas.setColor(open_ ? kAccent : kDisabled);
	const float cx = x + w + caret * 0.5f + 2.0f;
	const float cy = kLabelHeight * 0.5f;
	canvas.triangle(cx - 3.0f, cy + 2.0f, cx + 3.0f, cy + 2.0f, cx, cy - 1.5f);
}

// ---- SpectrumPanel ----------------------------------------------------------------------------------------------

float SpectrumPanel::rowHeight() {
	return kHeadHeight + 4.0f + Knob::columnHeight(kKnobFace) + 2.0f * kPanelPad;
}

float SpectrumPanel::rowHeightNow() const {
	return dense_ ? kHeadHeight + 4.0f + PillStack::kHeight + 2.0f * kPanelPad : rowHeight();
}

SpectrumPanel::SpectrumPanel(ParamBridge& bridge, WidgetRegistry& registry, const SoundSelection& selection,
                             Notifier* notifier)
    : SpectrumPanel(std::make_unique<BridgeSpectrumSource>(bridge, &registry), &bridge, &selection, registry, notifier) {}

SpectrumPanel::SpectrumPanel(SpectrumSource& source, WidgetRegistry& registry, Notifier* notifier, bool dense)
    : source_(source),
      registry_(registry),
      notifier_(notifier),
      master_(kSpectralOnKey, "Spectral", "Spectral processing on or off.", [this] { return source_.master(); },
              [](bool) {}),
      drawSwitch_("ui.spectral.draw", "Draw",
                  "The Draw tool: a press on the plot paints the curve instead of grabbing a handle.",
                  [this] { return drawTool_; }, [this](bool on) { drawTool_ = on; }) {
	dense_ = dense;
	// ITS FILTER SECTIONS KEEP FULL KNOBS TOO (backlog OG, by preference), as the Additive drawer's do; the other sections stay pill stacks.
	filterKnobs_ = dense;
	build();
}

SpectrumPanel::SpectrumPanel(std::unique_ptr<SpectrumSource> owned, ParamBridge* bridge, const SoundSelection* selection,
                             WidgetRegistry& registry, Notifier* notifier)
    : ownedSource_(std::move(owned)),
      source_(*ownedSource_),
      bridge_(bridge),
      selection_(selection),
      registry_(registry),
      notifier_(notifier),
      master_(kSpectralOnKey, "Spectral",
              "Spectral processing on or off. Off is the engine's own sound, and every setting is kept.",
              [bridge] { return bridge->spectralOn(); }, [bridge](bool on) { bridge->setSpectralOn(on); }),
      drawSwitch_("ui.spectral.draw", "Draw",
                  "The Draw tool: a press on the plot paints the selected sound's curve instead of grabbing a handle.",
                  [this] { return drawTool_; }, [this](bool on) { drawTool_ = on; }) {
	// THE DRAWER IS DENSE TOO (backlog OF: the plugin leads): the one SpectrumPanel in the Spectral window's mode, so the
	// conventions are shared rather than copied. Its FILTER section alone keeps full knobs (backlog OG, by preference).
	dense_ = true;
	filterKnobs_ = true;
	selected_ = std::min(selection_->slot(), kMaxSounds - 1); // opened on the selected sound, shown at once
	build();
	// A-PH (additive design §8): THE ENGINE'S PHASES, ENGINE-WIDE, so in the strip above the plot rather than in a sound's
	// panels. Phases as a ValueBox with its menu (every choice visible before one is applied: no cycling chip); Sub and Ceiling
	// beside it, greyed off Sub priority (updateContent).
	const auto box = [&](ParamId id, const char* caption, std::unique_ptr<device::Modulatable>& owner) {
		owner = std::make_unique<ParamControl>(*bridge_, findParam(static_cast<clap_id>(id)));
		return std::make_unique<ValueBox>(*owner, caption);
	};
	auto phases = box(ParamId::AdditivePhases, "Phases", phasesControl_);
	auto sub = box(ParamId::AdditiveSub, "Sub", subControl_);
	auto ceiling = box(ParamId::AdditiveCeiling, "Ceiling", ceilingControl_);
	phasesBox_ = phases.get();
	subBox_ = sub.get();
	ceilingBox_ = ceiling.get();
	// SUBSCRIBED, as the panel's chips are (the registry's watchers are how a param's move reaches its widget).
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditivePhases), phasesBox_));
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveSub), subBox_));
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveCeiling), ceilingBox_));
	std::vector<LeadItem> strip;
	strip.push_back(LeadItem { std::move(phases), kPhasesBoxWidth });
	strip.push_back(LeadItem { std::move(sub), kStripBoxWidth });
	strip.push_back(LeadItem { std::move(ceiling), kStripBoxWidth });
	addHeaderStrip(std::move(strip));
}

void SpectrumPanel::build() {
	// ONE SET PER SOUND (the source's slots: the drawer's eight, a device's one), each bound at construction to its own
	// sound's controls (the flank knobs' pattern): one panel per operator the source has — its switch, then its continuous
	// controls as knobs; Filter Mode and Link in their panels' heads.
	sets_.resize(source_.slots());
	for (size_t slot = 0; slot < sets_.size(); ++slot) {
		SoundSet& set = sets_[slot];
		const auto panel = [&](const char* name, SpectralControl sw, std::initializer_list<SpectralControl> knobs,
		                       int filter = -1) {
			if (!source_.has(sw)) {
				return false; // an operator this source does not have: no panel
			}
			Panel p;
			p.name = name;
			p.sw = sw;
			p.filter = filter;
			p.swControl = source_.control(slot, sw);
			p.chip = std::make_unique<SwitchChip>(*p.swControl, filter >= 0 ? kFilterChipLabel : name);
			source_.watch(slot, sw, p.chip.get(), watches_);
			addChild(p.chip.get());
			std::vector<SpectralControl> present;
			for (const SpectralControl c : knobs) {
				if (source_.has(c)) {
					present.push_back(c);
				}
			}
			// DENSE: the amounts as PillStacks of amount-only pills, three to a cell (two in the two-pill form); one left over
			// stays a knob. The section's chip stays its switch.
			size_t at = 0;
			while (dense_ && !(filterKnobs_ && filter >= 0) && present.size() - at >= 2) {
				const size_t take = std::min<size_t>(PillStack::kCount, present.size() - at);
				std::vector<SpectralControl> ids(present.begin() + static_cast<std::ptrdiff_t>(at),
				                                 present.begin() + static_cast<std::ptrdiff_t>(at + take));
				std::array<PillStack::Spec, PillStack::kCount> specs {};
				for (size_t k = 0; k < take; ++k) {
					p.owned.push_back(source_.control(slot, ids[k]));
					specs[k] = { p.owned.back().get(), nullptr, captionOf(ids[k]) };
				}
				// A SECOND STACK IN ONE SECTION NEEDS ITS OWN TITLE: the Vocode section's Low/High stack is its Range, not a second "Vocode".
				const std::string title = at > 0 && std::string(name) == "Vocode" ? std::string("Range") : std::string(name);
				auto stack = take == PillStack::kCount
				                 ? std::make_unique<PillStack>(std::move(specs), title)
				                 : std::make_unique<PillStack>(std::array<PillStack::Spec, 2> { specs[0], specs[1] }, title);
				addChild(stack.get());
				for (size_t k = 0; k < ids.size(); ++k) {
					source_.watch(slot, ids[k], &stack->pill(k), watches_);
				}
				p.stacks.emplace_back(std::move(ids), std::move(stack));
				at += take;
			}
			for (; at < present.size(); ++at) {
				const SpectralControl c = present[at];
				auto knob = source_.knob(slot, c, captionOf(c), notifier_);
				knob->setFaceDiameter(dense_ ? Knob::kEngineRailDiameter : kKnobFace);
				addChild(knob.get());
				p.knobs.emplace_back(c, std::move(knob));
			}
			set.panels.push_back(std::move(p));
			return true;
		};
		using C = SpectralControl;
		// DENSE: TILT AND ODD/EVEN SHARE ONE CELL, "Tone" — two FaderPills, each with its own power (a switch plus one amount
		// each, the pill's own case), under one title: both reweight the partials' levels across the spectrum.
		const auto tone = [&]() {
			if (!dense_ || !source_.has(C::TiltOn) || !source_.has(C::OddEvenOn)) {
				return false;
			}
			Panel p;
			p.kind = Panel::Kind::Tone;
			p.name = "Tone";
			p.owned.push_back(source_.control(slot, C::TiltAmount));
			p.owned.push_back(source_.control(slot, C::TiltOn));
			p.owned.push_back(source_.control(slot, C::OddEvenBalance));
			p.owned.push_back(source_.control(slot, C::OddEvenOn));
			std::array<PillStack::Spec, 2> specs { PillStack::Spec { p.owned[0].get(), p.owned[1].get(), "Tilt" },
				                                   PillStack::Spec { p.owned[2].get(), p.owned[3].get(), "Odd/Even" } };
			auto stack = std::make_unique<PillStack>(specs, "Tone");
			addChild(stack.get());
			for (size_t k = 0; k < 2; ++k) {
				const std::array<C, 2> amounts { C::TiltAmount, C::OddEvenBalance }, powers { C::TiltOn, C::OddEvenOn };
				source_.watch(slot, amounts[k], &stack->pill(k), watches_);
				source_.watch(slot, powers[k], &stack->pill(k), watches_);
			}
			p.stacks.emplace_back(std::vector<SpectralControl> { C::TiltAmount, C::OddEvenBalance }, std::move(stack));
			set.panels.push_back(std::move(p));
			return true;
		};
		// THE TWO FILTERS (F2, additive design §3.6): a panel each, only the selected tab's in the row (showFilterTab). Each head is
		// the tabs, the switch, the Mode and the routing chip; the routing's chip is greyed unless both filters are on.
		const bool twoFilters = source_.has(C::Filter2On);
		// THE ROUTING'S PLACE: a chip in a dense head; in the drawer a PAD in the knob row, after the knobs (the head holds the tabs,
		// the switch and five Mode segments, as much as the narrowest panel allows).
		const float filterHead = (twoFilters ? kFilterTabsWidth + 6.0f : 0.0f) + (dense_ ? kModeBoxWidth : kModeWidth)
		                         + (twoFilters && dense_ ? 6.0f + chipWidth(kRoutingChipLabel) : 0.0f);
		if (panel("Filter 1", C::FilterOn,
		          { C::FilterCutoff, C::FilterResonance, C::FilterSlope, C::FilterKeyTrack, C::FilterGain }, 0)) {
			set.panels.back().headExtra = filterHead;
		}
		if (panel("Filter 2", C::Filter2On,
		          { C::Filter2Cutoff, C::Filter2Resonance, C::Filter2Slope, C::Filter2KeyTrack, C::Filter2Gain }, 1)) {
			set.panels.back().headExtra = filterHead;
		}
		panel("Comb", C::CombOn, { C::CombSpacing, C::CombDepth, C::CombPhase });
		if (panel("Formant Shift", C::FormantShiftOn,
		          { C::FormantShiftLow, C::FormantShiftHigh, C::FormantShiftCrossover })) {
			set.panels.back().headExtra = kLinkWidth;
		}
		if (!tone()) {
			panel("Tilt", C::TiltOn, { C::TiltAmount });
			panel("Odd/Even", C::OddEvenOn, { C::OddEvenBalance });
		}
		// THE LATER FIVE (spectral-ops, 2026-10-12): the vowel, then the voice's own operators. Stretch and Shift are frequency
		// operators, drawn here because the panel exists only on Additive, where partials can move (design §3.1).
		panel("Vowel", C::FormantOn, { C::FormantVowel, C::FormantDepth });
		panel("Damp", C::DampOn, { C::DampTime, C::DampTilt });
		panel("Stretch", C::StretchOn, { C::StretchAmount });
		// PARTIALS IS A MODE CELL (backlog OH/OI), as the Spectral window's is: the dropdown the cull scheme, the
		// box the count. A source without the scheme keeps the Count knob. AFTER STRETCH, not after the vowel: the cell is wider than
		// the knob it replaced, and here the narrowest panel (374) still packs the row into five lines (MEASUREMENT case).
		if (source_.has(C::PartialsMode)) {
			// THE DEPTH IS A KNOB BESIDE THE CELL, as the Spectral window's is (backlog OF).
			if (panel("Partials", C::PartialsOn, { C::PartialsDepth })) {
				Panel& p = set.panels.back();
				p.owned.push_back(source_.control(slot, C::PartialsMode));
				std::vector<ZoneSelect::ZoneName> schemes;
				for (size_t i = 0; i < kSpectralPartialsModeNames.size(); ++i) {
					schemes.push_back({ kSpectralPartialsModeNames[i], kPartialsSchemeLines[i] });
				}
				// THE BOX SHOWN IS THE ONE ITS SCHEME READS: the count in Peaks and Harmonics, the set in Centers (OI).
				auto cell = std::make_unique<ModeValueCell>(
				    *p.owned.back(), "Partials", std::move(schemes), [this, slot](size_t param) {
					    const bool centers = steppedIndex(source_.value(slot, C::PartialsMode), kSpectralPartialsModeNames.size())
					                         == static_cast<size_t>(spectral::PartialsScheme::Centers);
					    return param == static_cast<size_t>(C::PartialsSet) ? centers : !centers;
				    });
				for (const C follower : { C::PartialsCount, C::PartialsSet }) {
					p.owned.push_back(source_.control(slot, follower));
					ValueBox& box = cell->addFollower(static_cast<size_t>(follower), std::make_unique<ValueBox>(*p.owned.back(), ""));
					// SUBSCRIBED, as the knob was: the registry knows the controls are reachable, and a host move repaints them.
					source_.watch(slot, follower, &box, watches_);
				}
				source_.watch(slot, C::PartialsMode, cell.get(), watches_);
				cell->setFaceDiameter(dense_ ? Knob::kEngineRailDiameter : kKnobFace);
				cell->refresh();
				set.partialsCell = cell.get();
				const float w = std::max(cell->minimumWidth(), kKnobColumn); // a knob's column, unless a scheme's name needs more
				addChild(cell.get());
				p.cells.push_back(LeadItem { std::move(cell), w, Knob::columnHeight(dense_ ? Knob::kEngineRailDiameter : kKnobFace) });
			}
		}
		else {
			panel("Partials", C::PartialsOn, { C::PartialsCount });
		}
		panel("Shift", C::ShiftOn, { C::ShiftHz });
		// OE (additive design §9.4): THE VOCODER, LAST IN THE ROW — the sound's own Depth, Flatten, Gate, Range and Bandwidth, the
		// Spacing and Widths cells, and before them the engine's analysis (a panel shown only while some sound vocodes). Last, so at
		// the narrowest width it packs beside Shift and the row keeps the lines it had.
		if (panel("Vocode", C::VocodeOn, { C::VocodeDepth, C::VocodeFlatten, C::VocodeGate, C::VocodeLow, C::VocodeHigh,
		                                    C::VocodeBandwidth })
		    && bridge_ != nullptr) {
			addVocodeAnalysis(slot, set);
		}
		// THE DRAWN CURVE: its switch is the source's (the drawer's stored state, a device's control); its other controls here are
		// Flatten and the Draw tool, and the plot draws it.
		set.curveSwitch = source_.curveSwitch(slot);
		set.curveChip = std::make_unique<SwitchChip>(*set.curveSwitch, "Curve");
		addChild(set.curveChip.get());
		if (!dense_) { // DENSE: no curve panel; its switch, Flatten and Draw sit on the plot (below)
			Panel p;
			p.kind = Panel::Kind::Curve;
			p.name = "Curve";
			set.panels.push_back(std::move(p));
		}
		// FILTER MODE: all five on the face (a SegmentedControl, never a cycling chip — design §3.2), the param road; each filter's.
		if (source_.has(C::FilterMode) && dense_) {
			// DENSE: A ValueBox WITH ITS MENU (the house's other non-cycling choice: all five on demand, before anything moves).
			set.modeControl = source_.control(slot, C::FilterMode);
			set.modeBox = std::make_unique<ValueBox>(*set.modeControl, "Mode");
			set.modeBox->setCompact(true);
			source_.watch(slot, C::FilterMode, set.modeBox.get(), watches_); // a host move repaints the box, as it did the segments
			addChild(set.modeBox.get());
		}
		else if (source_.has(C::FilterMode)) {
			set.mode = source_.segmented(slot, C::FilterMode, kModeSegments, notifier_);
			addChild(set.mode.get());
		}
		if (source_.has(C::Filter2Mode) && dense_) {
			set.mode2Control = source_.control(slot, C::Filter2Mode);
			set.modeBox2 = std::make_unique<ValueBox>(*set.mode2Control, "Mode");
			set.modeBox2->setCompact(true);
			source_.watch(slot, C::Filter2Mode, set.modeBox2.get(), watches_);
			addChild(set.modeBox2.get());
		}
		else if (source_.has(C::Filter2Mode)) {
			set.mode2 = source_.segmented(slot, C::Filter2Mode, kModeSegments, notifier_);
			addChild(set.mode2.get());
		}
		// THE TABS AND THE ROUTING: a pair of each, one in each filter panel's head, over one control each.
		if (twoFilters) {
			if (!filterTabControl_) {
				filterTabControl_ = std::make_unique<FilterTabControl>(source_, [this] { showFilterTab(source_.selectedFilter()); });
			}
			for (size_t f = 0; f < 2; ++f) {
				set.tabs[f] = std::make_unique<FilterTabs>(*filterTabControl_, registry_, kFilterTabNames, notifier_);
				addChild(set.tabs[f].get());
			}
			if (source_.has(C::FilterRouting)) {
				set.routingControl = source_.control(slot, C::FilterRouting);
				for (size_t f = 0; f < 2; ++f) {
					set.routing[f] = std::make_unique<SwitchChip>(*set.routingControl, kRoutingChipLabel);
					if (!dense_) {
						set.routing[f]->setPresentation(SwitchChip::Presentation::Pad);
						set.routing[f]->setFaceDiameter(kKnobFace);
					}
					source_.watch(slot, C::FilterRouting, set.routing[f].get(), watches_);
					addChild(set.routing[f].get());
				}
			}
		}
		if (source_.has(C::FormantShiftLink)) {
			set.linkControl = source_.control(slot, C::FormantShiftLink);
			set.linkChip = std::make_unique<SwitchChip>(*set.linkControl, "Link");
			source_.watch(slot, C::FormantShiftLink, set.linkChip.get(), watches_);
			addChild(set.linkChip.get());
		}
		showSet(slot, slot == selected_);
	}
	// THE FILTER TAB THE SOURCE HOLDS (a device's may have been chosen in its card before this window opened).
	filterTab_ = source_.selectedFilter();
	for (SoundSet& set : sets_) {
		for (Panel& p : set.panels) {
			if (p.filter >= 0) {
				p.hidden = p.filter != filterTab_;
			}
		}
	}
	for (size_t slot = 0; slot < sets_.size(); ++slot) {
		showSet(slot, slot == selected_);
	}

	// THE KNOB COLUMN'S FLOOR, MEASURED: the widest caption as the knob draws it (Knob::captionWidth, its face and
	// tracking) and the margin a column keeps each side of its caption (Knob::kCaptionMargin), so a squeezed row never cuts
	// a caption — a cut label is as much a defect as a clipped control (decided 2026-10-11). Every set's captions
	// are the same words, so the first set's are measured.
	measureColumn();

	// THE PLOT (D2), and the Draw tool's chip in the curve's panel head.
	plot_ = std::make_unique<SpectrumPlot>(source_);
	addChild(plot_.get());
	drawChip_ = std::make_unique<SwitchChip>(drawSwitch_, "Draw");
	// NOT A PARAM CONTROL (backlog QK): Draw is the panel's editing mode (`StoredSwitch`), not a destination.
	drawChip_->setShowsModulation(false);
	addChild(drawChip_.get());
	if (dense_) {
		// THE CURVE'S OVERLAY, ADDED AFTER THE PLOT so it paints over it (visage paints children in order): each set's Curve
		// switch, Flatten and the Draw tool on the plot's corner.
		for (SoundSet& set : sets_) {
			removeChild(set.curveChip.get());
			addChild(set.curveChip.get());
		}
		flattenButton_ = std::make_unique<SpectrumFlattenButton>([this] { flatten(); });
		addChild(flattenButton_.get());
	}

	// FLATTEN, drawn by the panel (one action, not a control): every band of the selected sound's curve back to unity,
	// one edit.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (flattenEnabled_ && flattenBounds_.contains(e.position)) {
			flatten();
		}
	};
}

void SpectrumPanel::measureColumn() {
	float widest = 0.0f;
	for (const Panel& p : sets_[0].panels) {
		for (const auto& [c, knob] : p.knobs) {
			widest = std::max(widest, knob->captionWidth());
		}
	}
	// AND THE ROUTING PAD'S CAPTION (F2): a column of the knob row too, so it is never cut either.
	if (!dense_ && sets_[0].routing[0] != nullptr) {
		widest = std::max(widest, trackedWidth(font(kKnobLabelSize), "PARALLEL", kKnobLabelTracking));
	}
	minColumn_ = std::ceil(widest + 2.0f * Knob::kCaptionMargin);
	if (dense_) {
		minColumn_ = std::max(minColumn_, PillStack::kMinWidth); // a stack's captions and readout need its narrowest cell
	}
	naturalColumn_ = std::max(kKnobColumn, minColumn_);
}

void SpectrumPanel::addLeadingPanel(const char* name, std::vector<LeadItem> head, std::vector<std::unique_ptr<Knob>> knobs,
                                    std::vector<std::unique_ptr<PillStack>> stacks, std::vector<LeadItem> cells) {
	// ONE SET (a window over another source has one): its panel first in the row, laid out and drawn as the operators' are.
	SoundSet& set = sets_[0];
	Panel p;
	p.kind = Panel::Kind::Lead;
	p.name = name;
	for (LeadItem& item : head) {
		addChild(item.frame.get());
		p.lead.push_back(std::move(item));
	}
	for (auto& knob : knobs) {
		knob->setFaceDiameter(dense_ ? Knob::kEngineRailDiameter : kKnobFace);
		addChild(knob.get());
		p.knobs.emplace_back(SpectralControl::kCount, std::move(knob));
	}
	for (auto& stack : stacks) {
		addChild(stack.get());
		p.stacks.emplace_back(std::vector<SpectralControl> {}, std::move(stack));
	}
	for (LeadItem& cell : cells) {
		addChild(cell.frame.get());
		p.cells.push_back(std::move(cell));
	}
	// AFTER THE LEADING PANELS ALREADY ADDED, so they keep the order they were added in.
	size_t at = 0;
	while (at < set.panels.size() && set.panels[at].kind == Panel::Kind::Lead) {
		++at;
	}
	set.panels.insert(set.panels.begin() + static_cast<std::ptrdiff_t>(at), std::move(p));
	measureColumn();
}

void VocodeAnalysisColumn::draw(visage::Canvas& canvas) {
	// THE CELL'S CAPTION, dim, on the bottom line where a stack's title sits (PillStack::draw): "Analysis" names the controls above it.
	canvas.setColor(kMuted);
	canvas.text(kCaption, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, height() - PillStack::kReadoutHeight, width(),
	            PillStack::kReadoutHeight);
}

void SpectrumPanel::addVocodeAnalysis(size_t slot, SoundSet& set) {
	// OE2 SIBILANCE, A KNOB IN THE VOCODE PANEL (a lone amount stays a knob; the engine's one layer, greyed with the analysis): the
	// analysis panel beside it is as wide as the narrowest row allows.
	{
		auto knob = paramKnob(*bridge_, registry_, findParam(static_cast<clap_id>(ParamId::AdditiveVocodeSibilance)), "Sibilance", notifier_);
		knob->setFaceDiameter(Knob::kEngineRailDiameter);
		set.vocodeSibilance = knob.get();
		addChild(knob.get());
		// AS WIDE AS ITS CAPTION NEEDS (no caption is ever cut, decided 2026-10-11).
		const float w = std::max(kKnobColumn, knob->captionWidth() + 2.0f * Knob::kCaptionMargin);
		set.panels.back().cells.push_back(LeadItem { std::move(knob), w, Knob::columnHeight(Knob::kEngineRailDiameter) });
	}
	// THE BANDS AND THE ENGINE'S ANALYSIS, A PANEL OF THEIR OWN BEFORE THE VOCODE PANEL (a chipless one: the cells' captions name
	// them; greyed by sync), in the row only while some sound vocodes (sync). Spacing and Widths are the engine's zones read by this
	// sound's values; the analysis is the engine's. With the Vocode panel they would not fit the narrowest row.
	Panel analysis;
	analysis.kind = Panel::Kind::Cells;
	analysis.name = "Analysis";
	analysis.hidden = !vocodeAnalysisShown_;
	const auto control = [&](ParamId id) {
		analysis.owned.push_back(std::make_unique<ParamControl>(*bridge_, findParam(static_cast<clap_id>(id))));
		return analysis.owned.back().get();
	};
	// THE ZONE CELLS BELONG TO THE VOCODE PANEL (the chip heads the whole section): Spacing and Widths read THIS
	// sound's values and grey with its Vocode switch, so they sit under its chip, after Sibilance. The analysis panel keeps what is engine-wide.
	Panel& p = set.panels.back();
	// OE2: SPACING AND WIDTHS, THE SPECTRAL WINDOW'S MODE CELLS — the dropdown the engine's zone, the box the one this sound's value
	// that zone reads (its amount in a named zone, its Free coordinate in Free, none in Even).
	const auto zoneCell = [&](ParamId mode, const char* caption, std::span<const char* const> names, std::span<const char* const> lines,
	                          SpectralControl amount, SpectralControl free, size_t freeIndex) {
		device::Modulatable* modeControl = control(mode);
		std::vector<ZoneSelect::ZoneName> zones;
		for (size_t i = 0; i < names.size(); ++i) {
			zones.push_back({ names[i], lines[i] });
		}
		auto cell = std::make_unique<ModeValueCell>(*modeControl, caption, std::move(zones), [this, mode, amount, free, freeIndex](size_t param) {
			const size_t z = steppedIndex(bridge_->value(static_cast<clap_id>(mode)), freeIndex + 1);
			return param == static_cast<size_t>(free) ? z == freeIndex : param == static_cast<size_t>(amount) && z > 0 && z < freeIndex;
		});
		for (const SpectralControl follower : { amount, free }) {
			p.owned.push_back(source_.control(slot, follower));
			ValueBox& box = cell->addFollower(static_cast<size_t>(follower), std::make_unique<ValueBox>(*p.owned.back(), ""));
			source_.watch(slot, follower, &box, watches_);
		}
		watches_.push_back(registry_.watchToken(static_cast<clap_id>(mode), cell.get()));
		cell->setFaceDiameter(Knob::kEngineRailDiameter);
		cell->refresh();
		ModeValueCell* raw = cell.get();
		const float w = std::max(cell->minimumWidth(), kKnobColumn);
		addChild(cell.get());
		p.cells.push_back(LeadItem { std::move(cell), w, Knob::columnHeight(Knob::kEngineRailDiameter) });
		return raw;
	};
	set.vocodeSpacing = zoneCell(ParamId::AdditiveVocodeSpacing, "Spacing", kAdditiveVocodeSpacingNames, fx::spectral::kZoneDescriptions,
	                             SpectralControl::VocodeSpacingAmount, SpectralControl::VocodeSpacingFree, 6);
	set.vocodeWidths = zoneCell(ParamId::AdditiveVocodeWidths, "Widths", kAdditiveVocodeWidthsNames, fx::spectral::kWidthZoneDescriptions,
	                            SpectralControl::VocodeWidthsAmount, SpectralControl::VocodeWidthsFree, 5);
	device::Modulatable* resolution = control(ParamId::AdditiveVocodeResolution);
	device::Modulatable* stereo = control(ParamId::AdditiveVocodeStereo);
	device::Modulatable* freeze = control(ParamId::AdditiveVocodeFreeze);
	device::Modulatable* keyTrack = control(ParamId::AdditiveVocodeKeyTrack);
	auto column = std::make_unique<VocodeAnalysisColumn>(*resolution, *stereo, *freeze, *keyTrack);
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveVocodeStereo), &column->stereo()));
	// SUBSCRIBED, as the strip's boxes are: a host move reaches the widget through the registry's watchers.
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveVocodeResolution), &column->resolution()));
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveVocodeFreeze), &column->freeze()));
	watches_.push_back(registry_.watchToken(static_cast<clap_id>(ParamId::AdditiveVocodeKeyTrack), &column->keyTrack()));
	set.vocodeColumn = column.get();
	addChild(column.get());
	analysis.cells.push_back(LeadItem { std::move(column), kVocodeColumnWidth, PillStack::kHeight });
	const std::array<std::pair<ParamId, const char*>, PillStack::kCount> times { {
	    { ParamId::AdditiveVocodeBands, "Bands" },
	    { ParamId::AdditiveVocodeAttack, "Attack" },
	    { ParamId::AdditiveVocodeRelease, "Release" },
	} };
	std::array<PillStack::Spec, PillStack::kCount> specs {};
	for (size_t k = 0; k < times.size(); ++k) {
		specs[k] = { control(times[k].first), nullptr, times[k].second };
	}
	auto stack = std::make_unique<PillStack>(std::move(specs), "Follower");
	for (size_t k = 0; k < times.size(); ++k) {
		watches_.push_back(registry_.watchToken(static_cast<clap_id>(times[k].first), &stack->pill(k)));
	}
	set.vocodeTimes = stack.get();
	addChild(stack.get());
	analysis.cells.push_back(LeadItem { std::move(stack), std::max(PillStack::kMinWidth, kKnobColumn), PillStack::kHeight });
	// BEFORE THE VOCODE PANEL, which build() has just added (the analysis feeds it, left to right).
	set.panels.insert(set.panels.end() - 1, std::move(analysis));
}

void SpectrumPanel::addHeaderStrip(std::vector<LeadItem> items) {
	for (LeadItem& item : items) {
		addChild(item.frame.get());
		strip_.push_back(std::move(item));
	}
}

SpectrumPanel::~SpectrumPanel() {
	// EVERY ROAD OUT OF THE PANEL ENDS HERE — ✕, Esc, the tab, the editor dismissing it off Additive, the editor closing —
	// so this is where the engine stops publishing a plot nobody draws (ParamBridge::plotWanted).
	if (bridge_ != nullptr) {
		bridge_->setPlotWanted(false);
	}
}

std::string SpectrumPanel::headerTitle() const {
	return std::string("Spectrum \xe2\x80\x94 Sound ") + static_cast<char>('A' + static_cast<int>(selected_)); // —
}

bool SpectrumPanel::updateContent() {
	// WANTED WHILE SHOWN: the tick runs only while the panel is presented, and a fresh open asks for a fresh frame
	// (ParamBridge::setPlotWanted's false -> true).
	bridge_->setPlotWanted(true);
	const size_t selected = selection_->slot();
	const OscType osc = oscTypeFromValue(bridge_->value(static_cast<clap_id>(ParamId::OscType)));
	// LIVE: Additive, and the selected sound in the constellation (a parked sound's settings buy nothing, as its flank
	// knobs' do not) — Explore's own rule, asked of the bridge.
	const bool inConstellation =
	    steppedIndex(bridge_->value(static_cast<clap_id>(soundParam(std::min(selected, kMaxSounds - 1), SoundField::Enable))),
	                 kEnableNames.size())
	    != 0;
	// A-PH: PHASES LIVE ON ADDITIVE; SUB AND CEILING ONLY UNDER SUB PRIORITY, where the engine reads them (characterFromParams).
	const bool additive = osc == OscType::Additive;
	const bool subPriority = additive
	                         && steppedIndex(bridge_->value(static_cast<clap_id>(ParamId::AdditivePhases)), kAdditivePhaseNames.size())
	                                == static_cast<size_t>(partials::PhaseMode::SubPriority);
	if (phasesBox_ != nullptr) {
		phasesBox_->setEnabled(additive);
		subBox_->setEnabled(subPriority);
		ceilingBox_->setEnabled(subPriority);
	}
	return sync(selected, additive && inConstellation);
}

bool SpectrumPanel::deviceEnabled() const {
	return source_.master();
}

std::optional<bool> SpectrumPanel::toggleDeviceEnable() {
	master_.setValue(source_.master() ? 0.0 : 1.0); // the stored switch's road: one step of the editor's undo
	return source_.master();
}

void SpectrumPanel::showSet(size_t slot, bool shown) {
	SoundSet& set = sets_[slot];
	const auto show = [shown](visage::Frame& frame) {
		if (frame.isVisible() != shown) {
			frame.setVisible(shown);
		}
	};
	for (Panel& p : set.panels) {
		if (p.chip) {
			show(*p.chip);
		}
		for (auto& [c, knob] : p.knobs) {
			show(*knob);
		}
		for (auto& [ids, stack] : p.stacks) {
			show(*stack);
		}
		for (LeadItem& cell : p.cells) {
			show(*cell.frame);
		}
		for (LeadItem& item : p.lead) {
			show(*item.frame);
		}
		if (p.hidden && shown) {
			// A HIDDEN PANEL'S WIDGETS STAY HIDDEN when its set is shown (setPanelHidden, or a filter tab not selected).
			if (p.chip) {
				p.chip->setVisible(false);
			}
			for (auto& [c, knob] : p.knobs) {
				knob->setVisible(false);
			}
			for (auto& [ids, stack] : p.stacks) {
				stack->setVisible(false);
			}
			for (LeadItem& cell : p.cells) {
				cell.frame->setVisible(false);
			}
			for (LeadItem& item : p.lead) {
				item.frame->setVisible(false);
			}
		}
	}
	if (set.linkChip) {
		show(*set.linkChip);
	}
	show(*set.curveChip);
	showFilterHead(slot, shown);
}

void SpectrumPanel::showFilterHead(size_t slot, bool setShown) {
	// EACH FILTER'S HEAD FRAMES SHOW WITH ITS PANEL: the set shown and the panel in the row (its tab selected).
	SoundSet& set = sets_[slot];
	bool inRow[2] = { false, false };
	for (const Panel& p : set.panels) {
		if (p.filter >= 0) {
			inRow[p.filter] = setShown && !p.hidden;
		}
	}
	const auto show = [](visage::Frame* frame, bool shown) {
		if (frame != nullptr && frame->isVisible() != shown) {
			frame->setVisible(shown);
		}
	};
	show(set.mode.get(), inRow[0]);
	show(set.modeBox.get(), inRow[0]);
	show(set.mode2.get(), inRow[1]);
	show(set.modeBox2.get(), inRow[1]);
	for (size_t f = 0; f < 2; ++f) {
		show(set.tabs[f].get(), inRow[f]);
		show(set.routing[f].get(), inRow[f]);
	}
}

void SpectrumPanel::showFilterTab(int filter) {
	filter = filter == 1 ? 1 : 0;
	if (filter == filterTab_) {
		return;
	}
	// A GESTURE ON THE PLOT STAYS ITS OWN (a handle carries its filter's controls); the row closes over the other filter's panel.
	filterTab_ = filter;
	for (size_t slot = 0; slot < sets_.size(); ++slot) {
		for (Panel& p : sets_[slot].panels) {
			if (p.filter < 0) {
				continue;
			}
			p.hidden = p.filter != filter;
			const bool shown = slot == selected_ && !p.hidden;
			p.chip->setVisible(shown);
			for (auto& [c, knob] : p.knobs) {
				knob->setVisible(shown);
			}
			for (auto& [ids, stack] : p.stacks) {
				stack->setVisible(shown);
			}
		}
		showFilterHead(slot, slot == selected_);
	}
	resized();
	redraw();
}

void SpectrumPanel::flatten() {
	const spectral::DrawnCurve before = source_.curve(selected_);
	source_.flattenCurve(selected_);
	const spectral::DrawnCurve after = source_.curve(selected_);
	if (history_ != nullptr && !(after == before)) {
		history_->record(std::make_unique<CurveEdit>(source_.curveWriter(selected_), selected_, before, after, "Flatten"));
	}
}

bool SpectrumPanel::on(size_t slot, SpectralControl control) const {
	return steppedIndex(source_.value(slot, control), kEnableNames.size()) != 0;
}

void SpectrumPanel::modulationBound(ModState* state, EditHistory* history) {
	history_ = history;
	plot_->setHistory(history);
	master_.setHistory(history);
	source_.bindModulation(state, history);
	for (SoundSet& set : sets_) {
		for (Panel& p : set.panels) {
			for (auto& [c, knob] : p.knobs) {
				knob->setModulation(state, history);
			}
			// AND THE CONTROLS A CELL'S BOX DRIVES (the Partials count, OH): a box asks its control for the arm state, as a knob does.
			for (auto& control : p.owned) {
				if (auto* owned = dynamic_cast<OwnedControl*>(control.get()); owned != nullptr) {
					owned->setModulation(state, history);
				}
				// AND A DEVICE'S CONTROL (the Spectral DEVICE's window, `DeviceSpectrumSource::control`): built for the panel before
				// any state was bound, it never heard of the arm, so its pills drew no glow for a source that could route to
				// them (backlog QK, 2026-11-08: the last of the sweep's deferrals, filed as "greyed" and not that at all).
				else if (auto* device = dynamic_cast<DeviceControl*>(control.get()); device != nullptr) {
					device->setModulation(state, history);
				}
			}
			// THE PILLS RE-SUBSCRIBE to the state their controls now answer from, so the glow repaints when the arm moves.
			for (auto& [controls, stack] : p.stacks) {
				for (size_t i = 0; stack != nullptr && i < stack->count(); ++i) {
					stack->pill(i).modulationChanged();
				}
			}
		}
	}
}

bool SpectrumPanel::sync(size_t selected, bool live) {
	bool repainted = false;
	selected = slotOf(selected);
	if (selected != selected_) {
		// A GESTURE OPEN ON THE PLOT IS THE OTHER SOUND'S: closed first (its stroke recorded, its handle's host gestures
		// ended), so nothing the hand does next lands on a sound the panel no longer shows.
		plot_->endGesture();
		showSet(selected_, false);
		selected_ = selected;
		showSet(selected_, true);
		redraw();
		repainted = true;
	}
	const bool master = source_.master();
	if (master != masterOn_) {
		masterOn_ = master;
		redraw(); // the panels' frames dim with it
		repainted = true;
	}
	using C = SpectralControl;
	// THE FILTER TAB, as the source holds it now: a handle grabbed on the plot, or the other view of a device, may have moved it.
	if (source_.selectedFilter() != filterTab_) {
		showFilterTab(source_.selectedFilter());
		repainted = true;
	}
	SoundSet& set = sets_[selected_];
	// EACH FILTER'S CONTROLS BY ITS OWN MODE AND OPEN END (spectralFilterOf); every other control reads filter 1's, which it ignores.
	const size_t modes[2] = { steppedIndex(source_.value(selected_, C::FilterMode), kSpectralFilterModeNames.size()),
		                      steppedIndex(source_.value(selected_, C::Filter2Mode), kSpectralFilterModeNames.size()) };
	const bool opens[2] = { source_.filterOpen(selected_, 0), source_.has(C::Filter2On) && source_.filterOpen(selected_, 1) };
	const size_t mode = modes[0];
	const bool linked = on(selected_, C::FormantShiftLink);
	const bool vowelSilent = !(source_.value(selected_, C::FormantDepth) > 0.0);
	const bool filterOpen = opens[0]; // at its open end the filter is off (Z7d)
	const auto inertOf = [&](SpectralControl c) {
		const int f = std::max(0, spectralFilterOf(c));
		return spectralControlInert(c, modes[f], linked, vowelSilent, opens[f]);
	};
	const bool curveOn = source_.curve(selected_).on;
	for (Panel& p : set.panels) {
		for (auto& [ids, stack] : p.stacks) {
			repainted = stack->refresh() || repainted; // a value can move under the readout with no hand on it
		}
		if (p.kind == Panel::Kind::Lead || p.kind == Panel::Kind::Cells) {
			continue; // the window's own: its greying is the window's
		}
		if (p.kind == Panel::Kind::Tone) {
			// TONE: each pill is its own switch (it draws dim while off); greyed only by the master, the sound, and inertness.
			for (auto& [ids, stack] : p.stacks) {
				for (size_t k = 0; k < ids.size(); ++k) {
					stack->pill(k).setActive(live && master && !spectralControlInert(ids[k], mode, linked, vowelSilent, filterOpen));
				}
			}
			continue;
		}
		const bool stored = p.kind == Panel::Kind::Curve;
		const bool opOn = stored ? curveOn : on(selected_, p.sw);
		SwitchChip& chip = stored ? *set.curveChip : *p.chip;
		chip.setEnabled(live && master);
		for (auto& [c, knob] : p.knobs) {
			// THE ENGINE AND THE SOUND'S SWITCH DECIDE WHETHER IT IS LIVE AT ALL; the switches on this face grey it.
			// THE ONE INERTNESS RULE the engine's blend also reads (spectralControlInert, params.h).
			const bool inert = inertOf(c);
			knob->setEnabled(live);
			knob->setActive(master && opOn && !inert);
		}
		// DENSE: AN AMOUNT-ONLY PILL GREYS AS ITS KNOB WOULD — the section's switch, the master, the sound, the inertness rule.
		for (auto& [ids, stack] : p.stacks) {
			for (size_t k = 0; k < ids.size(); ++k) {
				stack->pill(k).setActive(live && master && opOn && !inertOf(ids[k]));
			}
		}
	}
	if (set.mode) {
		set.mode->setEnabled(live);
		set.mode->setActive(master && on(selected_, C::FilterOn));
	}
	if (set.modeBox) {
		set.modeBox->setEnabled(live && master && on(selected_, C::FilterOn)); // it changes nothing while the filter is off
	}
	if (set.mode2) {
		set.mode2->setEnabled(live);
		set.mode2->setActive(master && on(selected_, C::Filter2On));
	}
	if (set.modeBox2) {
		set.modeBox2->setEnabled(live && master && on(selected_, C::Filter2On));
	}
	// THE PARTIALS CELL (OH) greys with its operator, as its knob did; a scheme moved shows the box it reads (the count, in both).
	if (set.partialsCell != nullptr) {
		set.partialsCell->setEnabled(live && master && on(selected_, C::PartialsOn));
		repainted = set.partialsCell->refresh() || repainted;
	}
	// OE: THE ENGINE'S ANALYSIS IS LIVE WHILE ANY SOUND VOCODES (Synth::vocodeLive), whichever sound is shown; Key Track only while
	// Freeze holds too (it moves only a held envelope).
	// OE2: THE ZONE CELLS grey with this sound's Vocode (they move only its bands); each shows the box its zone reads.
	for (ModeValueCell* cell : { set.vocodeSpacing, set.vocodeWidths }) {
		if (cell != nullptr) {
			cell->setEnabled(live && master && on(selected_, C::VocodeOn));
			repainted = cell->refresh() || repainted;
		}
	}
	if (set.vocodeColumn != nullptr) {
		bool any = false;
		for (size_t s = 0; s < source_.slots(); ++s) {
			any = any || on(s, C::VocodeOn);
		}
		// THE ANALYSIS PANEL IS IN THE ROW ONLY WHILE SOME SOUND VOCODES (the engine's settings: a control only another sound's
		// switch could wake is not drawn, the house's rule for engine settings), and the row is re-laid when that changes.
		if (any != vocodeAnalysisShown_) {
			vocodeAnalysisShown_ = any;
			for (SoundSet& each : sets_) {
				for (Panel& p : each.panels) {
					if (p.kind == Panel::Kind::Cells) {
						p.hidden = !any;
					}
				}
			}
			showSet(selected_, true);
			resized();
			repainted = true;
		}
		const bool analysis = live && master && any;
		const bool frozen = steppedIndex(bridge_->value(static_cast<clap_id>(ParamId::AdditiveVocodeFreeze)), kEnableNames.size()) != 0;
		set.vocodeColumn->setLive(analysis, analysis && frozen);
		if (set.vocodeSibilance != nullptr) {
			set.vocodeSibilance->setEnabled(live);
			set.vocodeSibilance->setActive(analysis); // the one layer acts while some sound vocodes
		}
		for (size_t k = 0; k < set.vocodeTimes->count(); ++k) {
			set.vocodeTimes->pill(k).setActive(analysis);
		}
		repainted = set.vocodeTimes->refresh() || repainted;
	}
	// THE ROUTING CHANGES NOTHING UNLESS BOTH FILTERS ARE ON (spectralRoutingInert); the tabs only choose what is shown.
	const bool routed = !spectralRoutingInert(on(selected_, C::FilterOn), source_.has(C::Filter2On) && on(selected_, C::Filter2On));
	for (size_t f = 0; f < 2; ++f) {
		if (set.routing[f]) {
			set.routing[f]->setEnabled(live && master && routed);
		}
		if (set.tabs[f]) {
			set.tabs[f]->setEnabled(live);
		}
	}
	if (dense_) {
		set.curveChip->setEnabled(live && master); // the curve's switch has no panel to be greyed with
	}
	if (set.linkChip) {
		set.linkChip->setEnabled(live && master && on(selected_, C::FormantShiftOn));
	}
	const bool flatten = live && master && curveOn;
	if (flatten != flattenEnabled_) {
		flattenEnabled_ = flatten;
		if (flattenButton_ != nullptr) {
			static_cast<SpectrumFlattenButton&>(*flattenButton_).setEnabled(flatten);
		}
		redraw();
		repainted = true;
	}
	// THE DRAW TOOL is live while the sound and the master are, curve on or off: a stroke switches the curve on (the plot).
	drawChip_->setEnabled(live && master);
	return plot_->sync(selected_, live, drawTool_) || repainted;
}

float SpectrumPanel::rowsHeight() const {
	const auto n = static_cast<float>(rows_);
	return n * rowHeightNow() + (n - 1.0f) * kPanelGap;
}

visage::Bounds SpectrumPanel::plotBounds() const {
	// A DENSE WINDOW'S HEADER STRIP sits above the plot.
	const float top = kPad + (strip_.empty() ? 0.0f : kStripHeight + kPanelGap);
	const float bottom = height() - kPad - rowsHeight() - kPanelGap;
	return { kPad, top, std::max(0.0f, width() - 2.0f * kPad), std::max(0.0f, bottom - top) };
}

void SpectrumPanel::resized() {
	layoutRow(); // first: how many rows decides the plot's height
	const visage::Bounds plot = plotBounds();
	plot_->setBounds(plot);
	// THE HEADER STRIP, item by item, each centred in the strip's height.
	float x = kPad;
	for (LeadItem& item : strip_) {
		const float h = item.height > 0.0f ? item.height : kStripHeight;
		item.frame->setBounds(x, kPad + (kStripHeight - h) * 0.5f, item.width, h);
		x += item.width + 6.0f;
	}
	if (dense_) {
		// THE CURVE'S OVERLAY ON THE PLOT'S TOP-LEFT CORNER, clear of the dB axis: its switch, Flatten, the Draw tool.
		const float y = plot.y() + 4.0f;
		float ox = plot.x() + kOverlayInset;
		for (SoundSet& set : sets_) {
			set.curveChip->setBounds(ox, y, chipWidth("Curve"), kHeadHeight);
		}
		ox += chipWidth("Curve") + 6.0f;
		flattenBounds_ = { ox, y, kFlattenWidth, kHeadHeight };
		flattenButton_->setBounds(flattenBounds_);
		drawChip_->setBounds(flattenBounds_.right() + 6.0f, y, kDrawWidth, kHeadHeight);
	}
	// **AND THE PANEL'S OWN PICTURE IS REPAINTED, because `draw` paints each panel's frame at the `p.bounds` `layoutRow`
	// just set.** A resize repaints through `setBounds`; a switch that adds or removes a panel (Vocode's analysis, a filter
	// tab, a leading panel) calls this directly, and the children moved while the frames behind them stayed where the last
	// paint put them — so the Additive engine's layout broke on enabling Vocode until the window was
	// resized. Here rather than at each caller, so a relayout is the whole of what a resize does by construction.
	redraw();
}

std::vector<float> SpectrumPanel::panelWidths(float column) const {
	std::vector<float> out;
	for (const size_t index : shownPanels()) {
		const Panel& p = sets_[0].panels[index];
		const bool stored = p.kind == Panel::Kind::Curve;
		float head = chipWidth(p.filter >= 0 ? kFilterChipLabel : p.name) + (p.headExtra > 0.0f ? 6.0f + p.headExtra : 0.0f)
		             + (stored ? 6.0f + kFlattenWidth + 6.0f + kDrawWidth : 0.0f);
		if (p.kind == Panel::Kind::Lead) {
			head = 0.0f;
			for (const LeadItem& item : p.lead) {
				head += (head > 0.0f ? 6.0f : 0.0f) + item.width;
			}
		}
		if (p.kind == Panel::Kind::Tone || p.kind == Panel::Kind::Cells) {
			head = 0.0f; // no chip and no head title: the stack's own caption names the cell, as every stack's does
			if (p.kind == Panel::Kind::Cells) {
				head = trackedWidth(font(kKnobLabelSize), kAnalysisHead, kControlTracking); // a chipless panel says whose it is
			}
		}
		// A FILTER PANEL'S ROUTING PAD is a column of the knob row (the drawer's; a dense head carries it as a chip).
		const bool routingPad = p.filter >= 0 && !dense_ && sets_[0].routing[static_cast<size_t>(p.filter)] != nullptr;
		float knobs = column * static_cast<float>(p.knobs.size() + p.stacks.size() + (routingPad ? 1 : 0)) + kCellGap * static_cast<float>(p.stacks.size() - (p.cells.empty() && p.knobs.empty() && !p.stacks.empty() ? 1 : 0) + (p.cells.empty() ? 0 : p.cells.size() - 1));
		for (const LeadItem& cell : p.cells) {
			knobs += cell.width;
		}
		out.push_back(std::max(head, knobs) + 2.0f * kPanelPad);
	}
	return out;
}

float SpectrumPanel::rowWidth(const std::vector<float>& widths, size_t from, size_t to) {
	float total = 0.0f;
	for (size_t i = from; i < to && i < widths.size(); ++i) {
		total += widths[i] + (i > from ? kPanelGap : 0.0f);
	}
	return total;
}

float SpectrumPanel::rowWidth(float column, size_t from, size_t to) const {
	return rowWidth(panelWidths(column), from, to);
}

void SpectrumPanel::layoutRow() {
	// EACH PANEL AS WIDE AS ITS HEAD OR ITS KNOBS, WHICHEVER IS WIDER, AND NEVER SPLIT. ONE ROW while it fits, its knob
	// columns squeezing from kKnobColumn down to the FLOOR (minColumn_: the widest caption, measured, and its margins — so
	// no caption is ever cut, decided 2026-10-11); past that the panels WRAP onto as many rows as they need (by
	// decision: wrap, never scroll, so no control is ever out of sight): the fewest rows that fit, split where the widest row
	// is narrowest, each row at the widest column that fits, and the plot gives up the height. EVERY SOUND'S SET IN THE
	// SAME PLACES, so a selection change is a visibility flip and nothing moves.
	const std::vector<size_t> shown = shownPanels();
	const size_t panels = shown.size();
	const float available = width() - 2.0f * kPad;
	// The widest column in [minColumn_, naturalColumn_] at which panels [from, to) fit `available`; the floor when none does.
	const auto columnFor = [&](size_t from, size_t to) {
		if (rowWidth(naturalColumn_, from, to) <= available) {
			return naturalColumn_;
		}
		float lo = minColumn_;
		float hi = naturalColumn_;
		for (int i = 0; i < 24; ++i) {
			const float mid = 0.5f * (lo + hi);
			(rowWidth(mid, from, to) <= available ? lo : hi) = mid;
		}
		return lo;
	};
	// EVERY WAY TO CUT THE PANELS INTO CONTIGUOUS ROWS (bit i set: a row ends after panel i), fewest rows first. A cut fits
	// when every row fits at the floor; among the fitting cuts with the fewest rows, the one whose widest row (at the
	// natural column) is narrowest. Six panels: 32 cuts, each a handful of sums, once per resize.
	const auto rowsOf = [panels](uint32_t cut) {
		std::vector<std::pair<size_t, size_t>> rows;
		size_t from = 0;
		for (size_t i = 0; i + 1 < panels; ++i) {
			if ((cut >> i) & 1u) {
				rows.emplace_back(from, i + 1);
				from = i + 1;
			}
		}
		rows.emplace_back(from, panels);
		return rows;
	};
	const uint32_t cuts = panels > 1 ? (1u << (panels - 1)) : 1u;
	// EACH PANEL'S WIDTH AT THE TWO COLUMNS THE CUTS ARE JUDGED AT, worked out once, not once per row of every cut.
	const std::vector<float> atFloor = panelWidths(minColumn_);
	const std::vector<float> atNatural = panelWidths(naturalColumn_);
	std::vector<std::pair<size_t, size_t>> chosen = rowsOf(cuts - 1); // a row per panel: the last resort
	bool fits = false;
	for (size_t count = 1; count <= panels && !fits && (wrap_ || count == 1); ++count) {
		float best = std::numeric_limits<float>::infinity();
		for (uint32_t cut = 0; cut < cuts; ++cut) {
			const auto rows = rowsOf(cut);
			if (rows.size() != count) {
				continue;
			}
			bool all = true;
			float widest = 0.0f;
			for (const auto& [from, to] : rows) {
				all = all && rowWidth(atFloor, from, to) <= available;
				widest = std::max(widest, rowWidth(atNatural, from, to));
			}
			if (all && widest < best) {
				best = widest;
				chosen = rows;
				fits = true;
			}
		}
	}
	if (!fits && !wrap_) {
		chosen = rowsOf(0); // the wrap off (a known-bad): one row, cut at the edge
	}
	struct Row {
		size_t from;
		size_t to;
		float column;
	};
	std::vector<Row> rows;
	for (const auto& [from, to] : chosen) {
		rows.push_back({ from, to, columnFor(from, to) });
	}
	rows_ = rows.size();
	fits_ = fits;
	oneRowFits_ = rowWidth(atFloor, 0, panels) <= available;
	for (SoundSet& set : sets_) {
		for (size_t r = 0; r < rows.size(); ++r) {
			const Row& row = rows[r];
			const std::vector<float> w = panelWidths(row.column);
			const float rowTop = height() - kPad - rowsHeight() + static_cast<float>(r) * (rowHeightNow() + kPanelGap);
			float x = kPad;
			for (size_t i = row.from; i < row.to; ++i) {
				Panel& p = set.panels[shown[i]];
				const bool stored = p.kind == Panel::Kind::Curve;
				p.bounds = { x, rowTop, w[i], rowHeightNow() };
				const float inner = p.bounds.x() + kPanelPad;
				const float headY = rowTop + kPanelPad;
				const float cw = chipWidth(p.filter >= 0 ? kFilterChipLabel : p.name);
				if (p.filter >= 0) {
					// A FILTER'S HEAD (F2): the tabs, its switch, its Mode, the routing chip.
					float hx = inner;
					if (set.tabs[static_cast<size_t>(p.filter)]) {
						set.tabs[static_cast<size_t>(p.filter)]->setBounds(hx, headY, kFilterTabsWidth, kHeadHeight);
						hx += kFilterTabsWidth + 6.0f;
					}
					p.chip->setBounds(hx, headY, cw, kHeadHeight);
					hx += cw + 6.0f;
					SegmentedControl* seg = p.filter == 0 ? set.mode.get() : set.mode2.get();
					ValueBox* box = p.filter == 0 ? set.modeBox.get() : set.modeBox2.get();
					if (seg != nullptr) {
						seg->setBounds(hx, headY, kModeWidth, kHeadHeight);
						hx += kModeWidth + 6.0f;
					}
					if (box != nullptr) {
						box->setBounds(hx, headY, kModeBoxWidth, kHeadHeight);
						hx += kModeBoxWidth + 6.0f;
					}
					if (set.routing[static_cast<size_t>(p.filter)] && dense_) {
						set.routing[static_cast<size_t>(p.filter)]->setBounds(hx, headY, chipWidth(kRoutingChipLabel), kHeadHeight);
					}
				}
				else if (p.kind == Panel::Kind::Lead) {
					// THE WINDOW'S OWN HEAD, item by item.
					float hx = inner;
					for (LeadItem& item : p.lead) {
						item.frame->setBounds(hx, headY, item.width, kHeadHeight);
						hx += item.width + 6.0f;
					}
				}
				else if (p.kind != Panel::Kind::Tone && p.kind != Panel::Kind::Cells) {
					SwitchChip& chip = stored ? *set.curveChip : *p.chip;
					chip.setBounds(inner, headY, cw, kHeadHeight);
				}
				if (p.sw == SpectralControl::FormantShiftOn && set.linkChip) {
					set.linkChip->setBounds(inner + cw + 6.0f, headY, kLinkWidth, kHeadHeight);
				}
				if (stored) {
					flattenBounds_ = { inner + cw + 6.0f, headY, kFlattenWidth, kHeadHeight };
					drawChip_->setBounds(flattenBounds_.right() + 6.0f, headY, kDrawWidth, kHeadHeight);
				}
				const float knobsTop = headY + kHeadHeight + 4.0f;
				const float face = dense_ ? Knob::kEngineRailDiameter : kKnobFace;
				// THE STACKS FIRST, THEN THE KNOBS (a dense window: the Vocode panel's three stacks, then Depth, its one knob).
				for (size_t k = 0; k < p.stacks.size(); ++k) {
					p.stacks[k].second->setBounds(inner + (row.column + kCellGap) * static_cast<float>(k), knobsTop, row.column, PillStack::kHeight);
				}
				for (size_t k = 0; k < p.knobs.size(); ++k) {
					p.knobs[k].second->setBounds(inner + row.column * static_cast<float>(p.stacks.size() + k) + kCellGap * static_cast<float>(p.stacks.size()), knobsTop, row.column,
					                             Knob::columnHeight(face));
				}
				float cx = inner + row.column * static_cast<float>(p.stacks.size() + p.knobs.size()) + kCellGap * static_cast<float>(p.stacks.size());
				for (LeadItem& cell : p.cells) {
					cell.frame->setBounds(cx, knobsTop, cell.width, cell.height > 0.0f ? cell.height : PillStack::kHeight);
					cx += cell.width + kCellGap;
				}
				cx -= p.cells.empty() ? 0.0f : kCellGap; // the routing pad (a non-dense filter) follows the last cell flush
				if (p.filter >= 0 && !dense_ && set.routing[static_cast<size_t>(p.filter)]) {
					set.routing[static_cast<size_t>(p.filter)]->setBounds(cx, knobsTop, row.column, Knob::columnHeight(face));
				}
				x += w[i] + kPanelGap;
			}
		}
	}
}

void SpectrumPanel::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, width(), height());
	// THE MASTER SWITCH OFF DIMS THE FRAMES BY BLENDING THEM INTO THE SURFACE, never by alpha (which would let what is behind
	// the panel show through).
	const float fade = masterOn_ ? 0.0f : 0.55f;
	const auto dim = [fade](uint32_t c) { return blendColour(c, kSurface, fade); };
	// THE PLOT IS ITS OWN CHILD (SpectrumPlot). EACH OPERATOR'S PANEL (every set's are in the same places).
	for (const Panel& p : sets_[selected_].panels) {
		if (p.hidden) {
			continue;
		}
		canvas.setColor(dim(kSurfaceRaised));
		canvas.roundedRectangle(p.bounds.x(), p.bounds.y(), p.bounds.width(), p.bounds.height(), kPanelRounding);
		canvas.setColor(dim(kLine));
		canvas.roundedRectangleBorder(p.bounds.x(), p.bounds.y(), p.bounds.width(), p.bounds.height(), kPanelRounding,
		                              1.0f);
		if (p.kind == Panel::Kind::Cells) {
			canvas.setColor(dim(kMuted));
			drawTracked(canvas, kAnalysisHead, font(kKnobLabelSize), kControlTracking, p.bounds.x() + kPanelPad, p.bounds.y() + kPanelPad + 3.0f,
			            kHeadHeight - 6.0f);
		}
	}
	// FLATTEN (a dense window's is its own button, over the plot).
	if (dense_) {
		return;
	}
	const visage::Bounds& f = flattenBounds_;
	canvas.setColor(flattenEnabled_ ? kLine : withAlpha(kLine, 0.5f));
	canvas.roundedRectangleBorder(f.x(), f.y(), f.width(), f.height(), kButtonRounding, 1.0f);
	const visage::Font& control = font(kKnobLabelSize);
	const std::string caption = "FLATTEN";
	const float cw = trackedWidth(control, caption, kControlTracking);
	canvas.setColor(flattenEnabled_ ? kMuted : kDisabled);
	drawTracked(canvas, caption, control, kControlTracking, f.x() + (f.width() - cw) * 0.5f, f.y() + 3.0f,
	            f.height() - 6.0f);
}

Knob* SpectrumPanel::knobForTest(size_t slot, SpectralControl control) const {
	for (const Panel& p : sets_[slotOf(slot)].panels) {
		for (const auto& [c, knob] : p.knobs) {
			if (c == control) {
				return knob.get();
			}
		}
	}
	return nullptr;
}

FaderPill* SpectrumPanel::pillForTest(size_t slot, SpectralControl control) const {
	for (const Panel& p : sets_[slotOf(slot)].panels) {
		for (const auto& [ids, stack] : p.stacks) {
			for (size_t k = 0; k < ids.size(); ++k) {
				// TONE's pills are their switches too: Tilt On and Odd/Even On find the pill that carries them.
				const bool power = p.kind == Panel::Kind::Tone
				                   && ((k == 0 && control == SpectralControl::TiltOn) || (k == 1 && control == SpectralControl::OddEvenOn));
				if (ids[k] == control || power) {
					return &stack->pill(k);
				}
			}
		}
	}
	return nullptr;
}

SwitchChip* SpectrumPanel::switchForTest(size_t slot, SpectralControl control) const {
	const SoundSet& set = sets_[slotOf(slot)];
	if (control == SpectralControl::FormantShiftLink) {
		return set.linkChip.get();
	}
	for (const Panel& p : set.panels) {
		if (p.kind == Panel::Kind::Operator && p.sw == control) {
			return p.chip.get();
		}
	}
	return nullptr;
}

visage::Frame* SpectrumPanel::frameForTest(size_t slot, SpectralControl control) const {
	const SoundSet& set = sets_[slotOf(slot)];
	if (control == SpectralControl::FilterMode) {
		return set.modeBox ? static_cast<visage::Frame*>(set.modeBox.get()) : set.mode.get();
	}
	if (control == SpectralControl::Filter2Mode) {
		return set.modeBox2 ? static_cast<visage::Frame*>(set.modeBox2.get()) : set.mode2.get();
	}
	if (control == SpectralControl::FilterRouting) {
		return set.routing[static_cast<size_t>(filterTab_)].get(); // the shown filter panel's chip
	}
	if (set.partialsCell != nullptr && control == SpectralControl::PartialsMode) {
		return set.partialsCell;
	}
	if (set.partialsCell != nullptr && (control == SpectralControl::PartialsCount || control == SpectralControl::PartialsSet)) {
		return set.partialsCell->boxFor(static_cast<size_t>(control));
	}
	if (set.vocodeSpacing != nullptr && (control == SpectralControl::VocodeSpacingAmount || control == SpectralControl::VocodeSpacingFree)) {
		return set.vocodeSpacing->boxFor(static_cast<size_t>(control));
	}
	if (set.vocodeWidths != nullptr && (control == SpectralControl::VocodeWidthsAmount || control == SpectralControl::VocodeWidthsFree)) {
		return set.vocodeWidths->boxFor(static_cast<size_t>(control));
	}
	if (SwitchChip* chip = switchForTest(slot, control); chip != nullptr) {
		return chip;
	}
	if (FaderPill* pill = pillForTest(slot, control); pill != nullptr) {
		return pill;
	}
	return knobForTest(slot, control);
}

bool SpectrumPanel::liveForTest(size_t slot, SpectralControl control) const {
	if (control == SpectralControl::FilterMode || control == SpectralControl::Filter2Mode) {
		const bool second = control == SpectralControl::Filter2Mode;
		if (const ValueBox* box = (second ? sets_[slotOf(slot)].modeBox2 : sets_[slotOf(slot)].modeBox).get(); box != nullptr) {
			return box->enabled();
		}
		const SegmentedControl& mode = second ? *sets_[slotOf(slot)].mode2 : *sets_[slotOf(slot)].mode;
		return mode.enabled() && mode.active();
	}
	if (control == SpectralControl::FilterRouting) {
		const SwitchChip* chip = sets_[slotOf(slot)].routing[static_cast<size_t>(filterTab_)].get();
		return chip != nullptr && chip->enabled();
	}
	if (const ModeValueCell* cell = sets_[slotOf(slot)].partialsCell;
	    cell != nullptr && (control == SpectralControl::PartialsMode || control == SpectralControl::PartialsCount
	                        || control == SpectralControl::PartialsSet)) {
		return cell->enabled();
	}
	// OE2: THE ZONE CELLS' VALUES are their cells' (which box shows is the zone's; the cell greys with the sound's Vocode).
	if (const ModeValueCell* cell = sets_[slotOf(slot)].vocodeSpacing;
	    cell != nullptr && (control == SpectralControl::VocodeSpacingAmount || control == SpectralControl::VocodeSpacingFree)) {
		return cell->enabled();
	}
	if (const ModeValueCell* cell = sets_[slotOf(slot)].vocodeWidths;
	    cell != nullptr && (control == SpectralControl::VocodeWidthsAmount || control == SpectralControl::VocodeWidthsFree)) {
		return cell->enabled();
	}
	if (const SwitchChip* chip = switchForTest(slot, control); chip != nullptr) {
		return chip->enabled();
	}
	if (const Knob* knob = knobForTest(slot, control); knob != nullptr) {
		return knob->enabled() && knob->active();
	}
	if (const FaderPill* pill = pillForTest(slot, control); pill != nullptr) {
		// TONE'S AMOUNT draws dim while its own switch is off (each pill is its own power), so it is live only with it on.
		if (control == SpectralControl::TiltAmount) {
			return pill->active() && on(slot, SpectralControl::TiltOn);
		}
		if (control == SpectralControl::OddEvenBalance) {
			return pill->active() && on(slot, SpectralControl::OddEvenOn);
		}
		return pill->active();
	}
	return false;
}

std::vector<visage::Frame*> SpectrumPanel::controlsForTest(size_t slot) const {
	const SoundSet& set = sets_[slotOf(slot)];
	std::vector<visage::Frame*> out;
	for (const Panel& p : set.panels) {
		if (p.kind == Panel::Kind::Lead) {
			for (const LeadItem& item : p.lead) {
				out.push_back(item.frame.get());
			}
		}
		else if (p.kind != Panel::Kind::Tone && p.kind != Panel::Kind::Cells) {
			out.push_back(p.kind == Panel::Kind::Curve ? static_cast<visage::Frame*>(set.curveChip.get()) : p.chip.get());
		}
		for (const auto& [ids, stack] : p.stacks) {
			out.push_back(stack.get()); // the cell (its pills are its own children: pillForTest)
		}
		for (const LeadItem& cell : p.cells) {
			out.push_back(cell.frame.get());
		}
		for (const auto& [c, knob] : p.knobs) {
			out.push_back(knob.get());
		}
	}
	if (set.mode) {
		out.push_back(set.mode.get());
	}
	if (set.modeBox) {
		out.push_back(set.modeBox.get());
	}
	for (visage::Frame* f : { static_cast<visage::Frame*>(set.mode2.get()), static_cast<visage::Frame*>(set.modeBox2.get()),
	                          static_cast<visage::Frame*>(set.tabs[0].get()), static_cast<visage::Frame*>(set.tabs[1].get()),
	                          static_cast<visage::Frame*>(set.routing[0].get()), static_cast<visage::Frame*>(set.routing[1].get()) }) {
		if (f != nullptr) {
			out.push_back(f);
		}
	}
	if (set.linkChip) {
		out.push_back(set.linkChip.get());
	}
	return out;
}

} // namespace feathers::ui

namespace feathers::ui {

std::vector<size_t> SpectrumPanel::shownPanels() const {
	std::vector<size_t> out;
	for (size_t i = 0; i < sets_[0].panels.size(); ++i) {
		if (!sets_[0].panels[i].hidden) {
			out.push_back(i);
		}
	}
	return out;
}

void SpectrumPanel::setLeadingPanelHidden(size_t lead, bool hidden) {
	size_t seen = 0;
	for (Panel& p : sets_[0].panels) {
		if (p.kind != Panel::Kind::Lead) {
			continue;
		}
		if (seen++ != lead) {
			continue;
		}
		if (p.hidden == hidden) {
			return;
		}
		p.hidden = hidden;
		for (auto& [c, knob] : p.knobs) {
			knob->setVisible(!hidden);
		}
		for (auto& [ids, stack] : p.stacks) {
			stack->setVisible(!hidden);
		}
		for (LeadItem& cell : p.cells) {
			cell.frame->setVisible(!hidden);
		}
		for (LeadItem& item : p.lead) {
			item.frame->setVisible(!hidden);
		}
		resized(); // the row closes over it, or opens for it
		redraw();
		return;
	}
}

} // namespace feathers::ui

namespace feathers::ui {

std::vector<visage::Frame*> SpectrumPanel::overlayForTest(size_t slot) const {
	if (!dense_) {
		return {};
	}
	return { sets_[slotOf(slot)].curveChip.get(), flattenButton_.get(), drawChip_.get() };
}

} // namespace feathers::ui

namespace feathers::ui {

std::vector<SpectrumPanel::SectionForTest> SpectrumPanel::sectionsForTest(size_t slot) const {
	const SoundSet& set = sets_[slotOf(slot)];
	std::vector<SectionForTest> out;
	for (const Panel& p : set.panels) {
		if (p.hidden) {
			continue; // out of the row entirely (the other filter's, the analysis while nothing vocodes)
		}
		SectionForTest s;
		s.name = p.name;
		s.bounds = p.bounds;
		if (p.kind == Panel::Kind::Operator) {
			s.chip = p.chip.get();
			s.frames.push_back(p.chip.get());
		}
		for (const LeadItem& item : p.lead) {
			s.frames.push_back(item.frame.get());
		}
		for (const auto& [ids, stack] : p.stacks) {
			s.frames.push_back(stack.get());
		}
		for (const LeadItem& cell : p.cells) {
			s.frames.push_back(cell.frame.get());
		}
		for (const auto& [c, knob] : p.knobs) {
			s.frames.push_back(knob.get());
		}
		if (p.filter >= 0) {
			const auto f = static_cast<size_t>(p.filter);
			for (visage::Frame* head : { static_cast<visage::Frame*>(set.tabs[f].get()), static_cast<visage::Frame*>(set.routing[f].get()),
			                             static_cast<visage::Frame*>(f == 0 ? set.modeBox.get() : set.modeBox2.get()),
			                             static_cast<visage::Frame*>(f == 0 ? set.mode.get() : set.mode2.get()) }) {
				if (head != nullptr) {
					s.frames.push_back(head);
				}
			}
		}
		if (p.sw == SpectralControl::FormantShiftOn && set.linkChip) {
			s.frames.push_back(set.linkChip.get());
		}
		out.push_back(std::move(s));
	}
	return out;
}

} // namespace feathers::ui
