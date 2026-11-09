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

#pragma once

#include "backplane/device_host.h"
#include "device/ears.h"
#include "mod/stereo/stereo_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// THE STEREO TOOL CARD (backlog KU, design §3, the sketch approved 2026-10-05).
//
//     [ SPLIT | WIDTH ]                  — which output the scope shows: a VIEW choice, every entry on the face
//     ┌───────────────────────────┐
//     │  L (mod-l) and R (mod-r)   │       — a two-trace scroll scope of that output, newest at the right edge
//     └───────────────────────────┘
//         [ the controls of THAT output ]   — In and Amount under Split; In, Width and Swap under Width
//
// **THE SCOPE IS THE DEVICE'S OWN ARITHMETIC, NEVER A SECOND COPY OF IT.** Each frame the card evaluates
// `mod::stereo::evaluate` — the one function the device's `pairAt` calls — over the controls as the ENGINE left
// them (`modulatedFieldAt`, and `modulatedFieldRAt` for In's right ear while a stereo route reaches it), and pushes
// the pair into a fixed ring the plot's width long. It holds nothing across threads beyond what the host publishes.
//
// **THE TABS ARE A `SegmentedControl` OVER A CONTROL THE CARD OWNS**, not a param: which output a hand is LOOKING
// at changes no sound, so it is stored nowhere and records no undo. Both entries are on the face, so it is not
// a multivalue toggle (CLAUDE.md). It is no `DeviceControl`, so no guard counts it among the device's controls.
//
// **A CONTROL IS SHOWN UNDER THE OUTPUT IT CHANGES** (the honesty rule, read per tab): every one of the four is
// reachable from some tab, and the enable is the head's.

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

class StereoToolCard final : public FxCard {
public:
	StereoToolCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	               Notifier* notifier = nullptr, ModState* mod = nullptr, EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	StereoToolCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	               EditHistory* = nullptr) = delete;
	~StereoToolCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	/// THE HOST'S SLOT MENU (Feathers' fx lane, a chain's lane), as the other modulator cards draw it since lane-mods;
	/// `drawSlotMenu` draws nothing where none is bound, so the strip is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::stereo::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kTabsHeight = 18.0f;
	static constexpr float kScopeHeight = 84.0f;
	static constexpr float kFaceDiameter = Knob::kEngineRailDiameter;
	/// THE RING'S LENGTH — one sample a frame, one pixel a sample; wider than any card the strip lays out.
	static constexpr size_t kScopeCapacity = 512;

	static constexpr float controlsHeight() { return Knob::columnHeight(kFaceDiameter); }
	static constexpr float bodyHeight() {
		return kGap + kTabsHeight + kGap + kScopeHeight + kGap + controlsHeight() + kGap;
	}
	/// OPEN, IN A VERTICAL LANE — `MathCard::openHeight`'s shape, for `ModPanel::bodyHeightOf`.
	static float openHeight(size_t outputRows);

	[[nodiscard]] SegmentedControl& tabs() const { return *tabs_; }
	[[nodiscard]] Knob& amount() const { return *amount_; }
	[[nodiscard]] Knob& in() const { return *in_; }
	[[nodiscard]] Knob& width() const { return *width_; }
	[[nodiscard]] SwitchChip& swap() const { return *swap_; }

	/// WHICH OUTPUT THE SCOPE SHOWS — the tab's value.
	[[nodiscard]] mod::stereo::Output shown() const noexcept { return shown_; }
	/// TURN THE TAB, as a click on it would (through the segmented control's own `select`).
	void show(mod::stereo::Output out);

	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// Every device control this card puts a widget on, asked of the widgets' own controls.
	[[nodiscard]] std::vector<mod::stereo::Param> addressedParamsForTest() const;

	/// **THE SCOPE'S SEAM** — the newest pair pushed, and how many are held. A case asserts the VALUE the traces
	/// are drawn from, not their pixels.
	[[nodiscard]] device::EarPair scopeLastForTest() const noexcept;
	[[nodiscard]] size_t scopeCountForTest() const noexcept { return count_; }
	/// The pair `count` frames back (0 = newest); `{}` past what is held.
	[[nodiscard]] device::EarPair scopeAtForTest(size_t back) const noexcept;

private:
	/// THE TAB'S CONTROL — a view choice the card owns. Writes go to `card_.show`'s bookkeeping, nowhere else.
	class ViewChoice final : public device::Modulatable {
	public:
		explicit ViewChoice(StereoToolCard& card) : card_(card) {}
		const device::ParamDesc& desc() const override;
		double value() const override { return static_cast<double>(card_.shown_); }
		void setValue(double value) override;
		device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override;

	private:
		StereoToolCard& card_;
	};

	device::Modulatable* enableModulatable() override;
	void setShown(mod::stereo::Output out);
	void layoutControls();
	/// THE PAIR THE SELECTED OUTPUT IS EMITTING NOW, from the engine's modulated controls.
	[[nodiscard]] device::EarPair evaluateNow() const;
	void push(device::EarPair pair) noexcept;
	[[nodiscard]] visage::Bounds scopeBounds() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mod::stereo::Output shown_ = mod::stereo::Output::Split;
	ViewChoice view_ { *this };
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(mod::stereo::Param::kCount)> controls_;
	std::unique_ptr<SegmentedControl> tabs_;
	std::unique_ptr<Knob> amount_;
	std::unique_ptr<Knob> in_;
	std::unique_ptr<Knob> width_;
	std::unique_ptr<SwitchChip> swap_;
	/// THE SCOPE'S RING: `head_` is where the next pair goes; `count_` how many are held (up to the capacity).
	std::array<device::EarPair, kScopeCapacity> ring_ {};
	size_t head_ = 0;
	size_t count_ = 0;
	/// HOW MANY OF THE NEWEST PAIRS ARE EQUAL — once it covers the plot, the picture is still and nothing repaints.
	size_t still_ = 0;
};

} // namespace feathers::ui
