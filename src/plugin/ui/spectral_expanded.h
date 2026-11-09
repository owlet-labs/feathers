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

#pragma once

#include "backplane/device_host.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/pill_stack.h"
#include "plugin/ui/spectral_card.h"
#include "plugin/ui/spectral_source.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <string>
#include <vector>

// THE SPECTRAL DEVICE'S WINDOW IS THE ADDITIVE SPECTRUM PANEL (a separate port of the formant drawer would diverge in UX;
// option A): `SpectrumPanel` itself over the device's source (spectral_source.h) —
// the same plot and handles, the same operators — DENSE (the plot gets a row back): a header strip above the
// plot (Resolution, Key Track, Poly, Offset, Mix), the operators' amounts in PillStacks, the Curve's switch, Flatten and Draw
// on the plot, and the window's own Vocode and Partials panels first in the row. The panel greys what is off, as it does for
// the drawer; the card hides (spectral_card.h).

namespace feathers::ui {

namespace detail {
/// THE SOURCE BEFORE THE PANEL (base-from-member): the panel's constructor builds its widgets over it, and it must outlive them.
struct SpectralWindowSource {
	SpectralWindowSource(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id) : deviceSource(host, registry, id) {}
	fx::spectral::DeviceSpectrumSource deviceSource;
};
} // namespace detail

class SpectralExpanded final : private detail::SpectralWindowSource, public SpectrumPanel {
public:
	SpectralExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	SpectralExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// The device window's own size floor: the device has five operator panels and its box, not the drawer's ten.
	static constexpr float kMinimumWidth = 760.0f;
	static constexpr float kMinimumHeight = 500.0f;
	static constexpr float kResolutionWidth = 96.0f;
	static constexpr float kBoxWidth = 64.0f;     ///< Offset and Mix in the header strip
	static constexpr float kHeadBoxWidth = 104.0f; ///< Carrier and Stereo, compact, in the Vocode head
	static constexpr float kModeCellWidth = 64.0f; ///< Zone and Widths' floor; each is at least as wide as its widest mode name
	/// OFFSET'S DRAG: 12 semitones a full drag (0.086 st/px), a fifth of that fine (Z7c: the Offset knob was too twitchy).
	static constexpr double kOffsetDragSpan = 12.0;
	static constexpr float kChipWidth = 40.0f;
	static constexpr float kVocodeChipWidth = 56.0f;
	static constexpr float kBadgeWidth = 96.0f;

	visage::Point minimumSize() const override { return { kMinimumWidth, kMinimumHeight }; }
	std::string title() const override { return fx::spectral::kTraits.name; }
	std::string headerTitle() const override { return fx::spectral::kTraits.name; }
	/// THE TICK: the panel's sync over the one set, live while the device is on, and the box greyed with it.
	bool updateContent() override;
	/// THE ENABLE IS THE DEVICE'S, as every device window's (not the drawer's master switch the panel otherwise answers for).
	[[nodiscard]] size_t deviceEnableControl() const override { return ExpandedView::deviceEnableControl(); }
	[[nodiscard]] bool deviceEnabled() const override { return ExpandedView::deviceEnabled(); }
	std::optional<bool> toggleDeviceEnable() override { return ExpandedView::toggleDeviceEnable(); }
	void modulationBound(ModState* mod, EditHistory* history) override;

	/// TEST SEAMS: the widget on `p` in the window's own panels (the box and the Vocode panel), or null.
	[[nodiscard]] visage::Frame* boxWidgetForTest(fx::spectral::Param p) const;
	[[nodiscard]] fx::spectral::DeviceSpectrumSource& sourceForTest() { return deviceSource; }

private:
	/// Builds one PillStack of amount-only pills over the device's controls `params`, captioned `captions`, titled `title`, and
	/// files each pill under its param for the test seam.
	std::unique_ptr<PillStack> makeStack(const std::vector<fx::spectral::Param>& params, const std::vector<const char*>& captions,
	                                     const char* title);

	ValueBox* resolution_ = nullptr; ///< a ValueBox with its menu (not a segmented row of three)
	SwitchChip* keyTrack_ = nullptr;
	SwitchChip* poly_ = nullptr;
	ValueBox* offset_ = nullptr;
	ValueBox* mix_ = nullptr;
	SwitchChip* vocode_ = nullptr;
	ValueBox* carrier_ = nullptr; ///< the vocoder's Carrier, its four in a menu
	SwitchChip* freeze_ = nullptr; ///< the vocoder's Freeze
	ValueBox* stereo_ = nullptr; ///< the vocoder's Stereo: Mono, Stereo, Spread, in a menu
	/// THE VOCODER'S ZONE AND WIDTHS, THE HOUSE'S MODE CELLS (Z7): the mode in the dropdown, the value it reads in the box — a
	/// named mode's fenced amount or Free's unbounded coordinate; Even reads neither.
	ModeValueCell* zone_ = nullptr;
	ModeValueCell* widthZone_ = nullptr;
	SidechainBadge* badge_ = nullptr;
	Knob* depth_ = nullptr; ///< the vocoder's Depth: the one full knob, its weight earned
	std::vector<PillStack*> vocodeStacks_; ///< Shape, Envelope, Range
	SwitchChip* partials_ = nullptr;
	ModeValueCell* partialsMode_ = nullptr; ///< the scheme (Peaks / Harmonics …) and its box, the count (OH)
	Knob* partialsDepth_ = nullptr;          ///< Depth, its own knob beside the cell
	/// Every pill the window's own panels hold, by its param.
	std::vector<std::pair<fx::spectral::Param, FaderPill*>> pills_;
};

} // namespace feathers::ui
