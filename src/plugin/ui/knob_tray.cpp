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

#include "plugin/ui/param_knob.h"
#include "plugin/ui/knob_tray.h"

#include "plugin/params.h"
#include "plugin/ui/theme.h"

namespace feathers::ui {

namespace {

/// The mock's footer row, in its order. The label is the mock's caption rather than the parameter's
/// name where the two differ: the row holds one gamma, so the mock calls GlobalGamma "Gamma".
///
/// `live` is the honesty rule, and it is not a style choice — it is whether the engine reads the
/// parameter today. Path, Pos X, Pos Y, Gamma, Weather, Attack, Release and Level do; Relief still
/// waits on the complexity field, and stays switched off until it has one.
///
/// POS X AND POS Y WERE SWITCHED OFF AND SHOULD NOT HAVE STAYED THAT WAY. In v1 they were "declared
/// and dormant" — real ids reserved for a constellation that did not exist — so a live knob would
/// have been a control that moved nothing, which the honesty rule forbids. Phase 2 built the
/// constellation: character_params.h:244 puts the puck at `onTour + (Pos X, Pos Y)` and hands it to
/// blendWeights, which decides every sound's share. They change the sound, so they are controls, and
/// the flag was left behind. It surfaced as params like Pos no longer being editable.
struct KnobSlot {
	ParamId id;
	const char* label;
	bool live;
};


// TEN SINCE V17: the master-scale Width joins beside Weather — its Character-family neighbour and
// the id right after it — so the two terrain amounts sit together the way the table declares them.
// Its `live` below is only the STARTING state; update() re-derives it from the published engine,
// which is what makes it the one knob here whose enablement is a property of the patch rather
// than of the build.
// NINE SINCE 2026-10-03: Attack and Release are gone completely (the
// envelope is the note's shape), and Voice Level, the envelope's destination, stands beside Level, now a fader.
constexpr KnobSlot kSlots[] = {
    {ParamId::Path, "Path", true},          {ParamId::PosX, "Pos X", true},
    {ParamId::PosY, "Pos Y", true},         {ParamId::Relief, "Relief", false},
    {ParamId::GlobalGamma, "Gamma", true},  {ParamId::Weather, "Weather", true},
    {ParamId::Width, "Width", true},        {ParamId::VoiceLevel, "Voice Level", true},
    {ParamId::Level, "Level", true},
};

} // namespace

KnobTray::KnobTray(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier,
                   std::function<size_t()> slotOf)
    : registry_(registry), bridge_(bridge) {
	for (const KnobSlot& slot : kSlots) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(slot.id));
		if (slot.id == ParamId::GlobalGamma) {
			// **ONE NUMBER, ONE BOX.** Gamma is a value box here and in Explore, not a coarse knob.
			// Gamma is one term of
			// `sliceHome`'s three, not the coarse half of a pair — `gamma_control.h` carries the
			// decomposition and the reversal.
			//
			// THE SLOT KEEPS ITS PLACE IN THE ROW. A box where a knob was is a rhythm change in a row
			// of ten and is to be reviewed at install; moving it as well would make two changes
			// out of one and leave neither reviewable.
			gammaControl_ = std::make_unique<GammaControl>(bridge, slotOf);
			// **NO CAPTION INSIDE THE BOX — the row names it, like everything else in the row.** The
			// first draft let the box caption itself, which collapsed two of the row's three tiers
			// into one rectangle and left the SHARED label row empty at this position: the row read
			// `PATH · POS X · POS Y · RELIEF · <gap> · WEATHER …`, and that gap was most of the
			// rhythm break. It also named this control in a different place from the other ten.
			//
			// So the box carries the NUMBER and the tray draws `GAMMA` on the label line — see
			// `KnobTray::draw`. The box stays obviously a box: a different shape and a different
			// affordance, saying "this one you type into", without leaving a hole where a name goes.
			gamma_ = std::make_unique<ValueBox>(*gammaControl_, "");
			gamma_->setCompact(true);
			// **AND EVERY PARAMETER IN THE SUM HAS TO TELL IT**, which is playbook 8i and the reason
			// this is three `watch` calls rather than none. A `ValueBox` is built from a
			// `Modulatable` and has no `clap_id` of its own to key on, so nothing watches for it: the
			// control would read the bridge perfectly while the box drew its construction-time number
			// forever, and the value case would pass throughout.
			//
			// THREE, BECAUSE THE READING IS A SUM. `GlobalGamma` moves it, and so does the SELECTED
			// sound's own Gamma — and which sound that is changes under the widget, so every slot's
			// Gamma is watched rather than the one that happened to be selected when the tray was
			// built. Watching too many costs a repaint nobody needed; watching too few costs a frozen
			// number, which is the failure that has no symptom until somebody photographs it.
			watches_.push_back(registry.watchToken(static_cast<clap_id>(slot.id), gamma_.get()));
			for (size_t sound = 0; sound < kMaxSounds; ++sound) {
				watches_.push_back(registry.watchToken(static_cast<clap_id>(soundParam(sound, SoundField::Gamma)),
				               gamma_.get()));
			}
			addChild(gamma_.get());
			cells_.push_back(gamma_.get());
			continue;
		}
		auto knob = paramKnob(bridge, registry, desc, slot.label, notifier);
		knob->setEnabled(slot.live);
		if (slot.id == ParamId::Width) {
			masterWidth_ = knob.get();
		}
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// The engine decides the master scale's enablement from the first frame, not from the first
	// tick that happens to arrive — a tray that opened enabled on a Stair patch would be wrong
	// until somebody moved something.
	update();
}

KnobTray::~KnobTray() {
	// **THE TWO FRAMES THIS TRAY REGISTERED BY HAND.** `paramKnob` builds knobs that unsubscribe
	// themselves (`~Knob`), but the gamma box was subscribed HERE — twice, once for the tray's own slot
	// and once per sound — and a frame subscribed by its owner can only be unsubscribed by its owner.
	//
	// `unwatch` sweeps every parameter rather than taking an id, so both registrations go with one
	// call. Found under ASan, where `~FrameWatchers`'s assert is live and `NDEBUG` does not silence it.
	registry_.unwatch(gamma_.get());
}

void KnobTray::update() {
	// ONE derivation, shared with the Explore width knob and the header — oscTypeFromValue — so no
	// two controls can disagree about which patches are flagship (V12c: on a frozen-engine patch
	// the master scale changes nothing, and the control must say so). Knob::setEnabled is guarded,
	// so the steady state costs one published read and a comparison.
	// AND ADDITIVE (2026-10-10) and BASIC SHAPES (backlog KV), whose two ears the synth builds from the same Width
	// (Synth's stereo predicate).
	const OscType osc = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	masterWidth_->setEnabled(osc == OscType::V5 || osc == OscType::Additive || osc == OscType::BasicShapes);
}

void KnobTray::setModulation(ModState* state, EditHistory* history) {
	for (std::unique_ptr<Knob>& knob : knobs_) {
		knob->setModulation(state, history);
	}
	// AND GAMMA'S BOX, WHICH A RELATIVE MACRO MAY REACH (backlog PQ): its route carries each turn into the
	// unbounded half the box reads. No other source may route there (`GammaControl::display`).
	if (gammaControl_ != nullptr) {
		gammaControl_->setModulation(state, history);
	}
}

void KnobTray::resized() {
	if (cells_.empty()) {
		return;
	}
	// The mock's `justify-content:space-evenly`: equal gaps either side of every cell.
	const float usable = width() - 2.0f * kPadX;
	const float each = usable / static_cast<float>(cells_.size());
	for (size_t i = 0; i < cells_.size(); ++i) {
		const float x = kPadX + static_cast<float>(i) * each;
		if (cells_[i] == gamma_.get()) {
			// REMEMBERED FOR `draw`, which needs the cell rather than the box to put the caption on
			// the shared label line — the box is inset inside it.
			gammaCell_ = { x, kPadTop, each, Knob::kHeight };
			// **A BOX IS NOT A COLUMN, AND GIVING IT ONE STRETCHED IT.** The first photograph of this
			// row had the gamma box filling a knob's whole `kHeight` — a tall empty rectangle with its
			// label and number stranded at the top. A `ValueBox` has a natural height and draws its own
			// caption; what it needs is that height and a place in the row, not a knob's slot pulled
			// over it.
			//
			// CENTRED IN THE CELL, at its own size and about two thirds of the width, so it reads as a
			// box standing where a knob stands rather than as a knob-shaped hole with text in it.
			// IN THE FACE BAND, where a knob's dial sits, so the row's first tier still has something
			// in it at this position and the caption below lands on the shared line.
			const float w = each * 0.68f;
			gamma_->setBounds(x + (each - w) * 0.5f,
			                  kPadTop + (Knob::kDiameter - ValueBox::kCompactHeight) * 0.5f, w,
			                  ValueBox::kCompactHeight);
			continue;
		}
		cells_[i]->setBounds(x, kPadTop, each, Knob::kHeight);
	}
}

void KnobTray::draw(visage::Canvas& canvas) {
	// NOTHING IS DRAWN HERE AT ALL, so the parameter is deliberately unused — see below for why the
	// two things this used to paint both belong somewhere else. Named-and-voided rather than left
	// unnamed, because the signature is an override and the name is what says WHICH argument a reader
	// is looking at.
	(void)canvas;
	// NO BACKGROUND FILL. The window already paints kVoid everywhere, so filling it again here bought
	// nothing and cost the footer's separator: this frame starts exactly at the line's row, so its
	// fill painted over it for the tray's whole width.
	//
	// The footer's `border-top` is NOT drawn here either, though it used to be. It belongs to the footer,
	// which is wider than this row: once the wave scope took the left third and the knobs started
	// after it, a border drawn across this frame's own width began at the scope's right edge and the
	// separator simply stopped for a third of the editor. It is drawn with the background now, where
	// the full width is known. See Gui::Impl::drawBackground.
	// **GAMMA'S NAME, ON THE SHARED LABEL LINE.** This function drew nothing at all until now and the
	// note above says why the two things it used to paint belong elsewhere — this is not one of those.
	// It is here because the caption belongs to the ROW rather than to the widget: every other control
	// is named on this line, and a box that named itself instead left the line with a hole in it.
	//
	// AT `Knob`'s OWN ARITHMETIC — `kDiameter + kGap`, the same place `Knob::draw` puts a caption —
	// rather than a number that happens to land nearby, so the ten names sit on one line by
	// construction.
	if (gamma_ != nullptr && gammaCell_.width() > 0.0f) {
		const visage::Font& face = font(kKnobLabelSize);
		canvas.setColor(kMuted);
		const std::string caption = upper("Gamma");
		const float w = trackedWidth(face, caption, kKnobLabelTracking);
		drawTracked(canvas, caption, face, kKnobLabelTracking,
		            gammaCell_.x() + (gammaCell_.width() - w) * 0.5f,
		            gammaCell_.y() + Knob::kDiameter + Knob::kGap, Knob::kLabelHeight);
	}
}

} // namespace feathers::ui
