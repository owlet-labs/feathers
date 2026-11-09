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

#include "plugin/gui_bridge.h"
#include "plugin/ui/gamma_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <functional>
#include <memory>
#include <vector>
#include <visage/ui.h>

// THE MOCK'S FOOTER ROW — ten of Feathers' own parameters, laid out evenly.
//
// LIFTED OUT OF knob.h, WHICH IS THE CARD FLOOR'S AND THIS IS NOT. `Knob` draws whatever
// `device::Modulatable` it is given and knows nothing about parameter spaces; this names ten
// `ParamId`s, reaches the bridge, and subscribes to a registry keyed by clap_id. Keeping the two in
// one header meant every device card that wanted a dial also compiled the synth's footer, its gamma
// decomposition and the whole plugin parameter table.
//
// Measured with a link probe before the split: `knob.cpp` was one of three floor files still pulling
// `plugin/params.h`, and this class plus `Knob`'s bridge constructor were the whole of why.

namespace feathers::ui {

/// The mock's footer: one row of knobs, evenly spread. TEN of them since V17 — the mock's fixed
/// nine revised in the design: the GLOBAL Width, the master scale over the per-sound widths,
/// joins beside Weather in its own Character family.
class KnobTray : public visage::Frame {
public:
	/// @param slotOf where the sound selection is NOW, for the one control in this row that is
	///        PER-SOUND. Optional: a tray built without it puts gamma's coarse half on sound 0, which
	///        is what a fixture with no selection should get rather than a crash.
	KnobTray(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier = nullptr,
	         std::function<size_t()> slotOf = {});
	~KnobTray() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// Hand the panel's state to every knob in the row. Forwarded rather than taken in the constructor
	/// for the reason Knob::setModulation states: the tray is built by an editor that has no business
	/// knowing which of its knobs a source may reach.
	void setModulation(ModState* state, EditHistory* history);

	/// Keep the master-scale knob's enablement in step with the published engine — the V12c honesty
	/// idiom the Explore width knob already follows, through the SAME derivation (oscTypeFromValue):
	/// on a frozen-engine patch the master scale changes nothing, and the control must say so.
	/// Called once a frame from the editor's tick, like the stages' updates; guarded, so a frame
	/// where nothing moved costs one read and a branch.
	void update();

	/// HOW MANY KNOBS THERE ACTUALLY ARE, for anything that needs to reason about the row — asked of
	/// the tray rather than restated as a constant, because the count has already changed once (nine
	/// in the mock, ten since V17's master Width) and a second copy of it is a second thing to update.
	size_t knobCount() const noexcept { return knobs_.size(); }

	/// HOW MANY CELLS THE ROW HAS, which is not the same number since gamma stopped being a knob.
	/// A layout cares about cells; an "is every knob wired?" sweep cares about knobs. Two questions,
	/// two answers, so neither has to be inferred from the other.
	size_t cellCount() const noexcept { return cells_.size(); }

	/// The mock's footer padding, `12px 20px 16px`, around the knob column.
	static constexpr float kPadX = 20.0f;
	static constexpr float kPadTop = 12.0f;
	static constexpr float kPadBottom = 16.0f;
	static constexpr float kHeight = Knob::kHeight + kPadTop + kPadBottom;

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	/// HELD SO `~KnobTray` CAN UNSUBSCRIBE the two frames it registered by hand.
	WidgetRegistry& registry_;
	ParamBridge& bridge_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	/// GAMMA, WHICH IS NOT A KNOB AND IS NOT A PAIR EITHER: gamma controls are coarse only and are value
	/// boxes, and Explore's device gamma is one number in one value box. It was a `ZoneCoordBox` for an
	/// hour; `gamma_control.h` records why the pairing was the
	/// wrong decomposition and what the code showed.
	std::unique_ptr<GammaControl> gammaControl_;
	std::unique_ptr<ValueBox> gamma_;
	/// The gamma cell's rectangle, so `draw` can put its caption on the row's shared label line.
	visage::Bounds gammaCell_;
	/// EVERY CELL IN ROW ORDER, knobs and the coordinate box alike — the layout walks this.
	std::vector<visage::Frame*> cells_;
	/// The one knob whose enablement is a function of the patch rather than of the build — the
	/// master scale. Null never: the ctor sets it while building the row.
	Knob* masterWidth_ = nullptr;
};

} // namespace feathers::ui