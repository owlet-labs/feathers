/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <functional>
#include <memory>
#include <string>
#include <visage/ui.h>

// **A TWO-STATE CONTROL IS A CHIP, NOT A BOX.**
//
// An enable is a toggle, not a dropdown. SKLITTER's Engage drew as a
// `ValueBox` reading `Engage / Off` — a bordered rectangle printing a word, identical to `Mode` beside
// it, which opens a list of eight. Two controls drawn the same and behaving differently is the
// affordance lying, and the honesty rule covers the SHAPE of a control as much as whether it is live.
//
// **THE BOX WAS NEVER EVEN A DROPDOWN.** `ValueBox::choiceMenuAvailable` already declines below three
// entries — *"A TWO-WAY SWITCH IS NOT A LIST... a menu to choose between Off and On is two clicks for
// what a nudge does"* — so the box printed a word, refused to open, and dragged instead. It looked
// like the one thing it would not do.
//
// **AND `value_box.h` ALREADY SAID WHAT THE ANSWER WAS**, in the same note: *"the switches on these
// cards are CHIPS anyway."* They were — hand-drawn inside `env_card.cpp` and `mseg_card.cpp`, each
// with its own bounds arithmetic and its own paint, reachable by no other surface. So a device that
// declared a two-way choice on a GENERIC surface got the box, because the chip was not a widget
// anybody could seat. This is that chip, extracted rather than invented: the colours, the radius and
// the lit/unlit rule are `env_card.cpp`'s, moved.

namespace feathers::ui {


class SwitchChip : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the chip — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	/// **THE CAPTION AND THE NAME IT STANDS FOR** (`ui::LabelFace`) — a pad's column caption, or a chip's own label where
	/// it prints one (an Off/On switch; a two-way picker prints its value instead and says nothing here).
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point) const override;
	/// `Knob::setFullName`: the name a shortened caption stands for; empty is the control's declared name.
	void setFullName(std::string name) { fullName_ = std::move(name); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	/// `modMarks`, for the glow-honesty sweep (`ui::ControlFace::modLookFor`): nothing unless `setShowsModulation`.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override {
		if (&c != &control_ || !showsModulation_ || !control_.display().isDestination) {
			return {};
		}
		const ModMarks m = modMarks();
		return { true, m.glow, m.ring };
	}

	/// `control` is borrowed and must outlive the chip — the card owns both. **AND THAT NOW COVERS THE
	/// RELEASE TOO:** a momentary destroyed while held writes min and closes its gesture on the way out
	/// (`cancelHold`), so the control must still be there when the chip's destructor runs.
	SwitchChip(device::Modulatable& control, std::string label);
	~SwitchChip() override;

	/// **WHAT A LEFT PRESS DOES.** `Latch` — the default, and every chip seated before macros had a type
	/// — flips the value on the press. `Momentary` holds it at the declared MAX for exactly as long as
	/// the hand is down: the press writes max and opens a gesture, the release writes min and closes it.
	/// **ONLY THE RELEASE LETS GO.** Sliding off the chip while held keeps it held, as a hardware
	/// momentary does — a finger that drifts off a button has not lifted.
	enum class Behaviour { Latch, Momentary };
	void setBehaviour(Behaviour behaviour) { behaviour_ = behaviour; }
	[[nodiscard]] Behaviour behaviour() const noexcept { return behaviour_; }

	/// **WHAT A RIGHT-CLICK DOES, and it is never "press the switch".** A right-click (or ctrl-click on
	/// a Mac — `shouldTriggerPopup`) goes here and nowhere else; with nothing set it does nothing. It
	/// used to toggle, because the press handler never asked which button — harmless while nothing
	/// wanted the right button, and wrong the moment a macro's menu (type, polarity, rename) lives on it:
	/// opening the menu would have fired the macro.
	void setPopup(std::function<void()> popup) { popup_ = std::move(popup); }
	/// **A LEFT PRESS THE OWNER TAKES** (Pattern's exclusive Solo: a press writes four fields as one step). Called for a
	/// plain left press of a usable chip, before the latch; true means it handled the press and the chip does nothing more.
	void setPressOverride(std::function<bool(const visage::MouseEvent&)> press) { pressOverride_ = std::move(press); }

	/// **HOW IT IS DRAWN — the one thing that differs between a chip in a list row and a button macro.**
	/// `Chip` is the list-row switch: a rounded strip printing its caption. `Pad` is the decision for a
	/// macro of type Latch or Momentary (2026-09-25): *a pad in the knob's face area, the size of the
	/// face, with the caption and the value below exactly as a knob has them* — so a row of macros keeps
	/// one rhythm whatever each one is, and the thing hit live is a face-sized target rather than a
	/// 16px strip. The pad carries no text; the caption is upper-cased under it like a knob's, and the
	/// reading under that says ON or OFF. **ONLY THE DRAWING CHANGES**: press, latch, momentary,
	/// right-click and `cancelHold` are the same code in both, which is why this is a presentation of
	/// the chip and not a second widget.
	enum class Presentation { Chip, Pad };
	void setPresentation(Presentation presentation);
	[[nodiscard]] Presentation presentation() const noexcept { return presentation_; }
	/// The pad's side — the knob face diameter it stands in for. Ignored by `Chip`.
	void setFaceDiameter(float diameter);
	[[nodiscard]] float faceDiameter() const noexcept { return faceDiameter_; }
	/// WHERE THE PAD IS DRAWN, in the chip's own coordinates: a square of the face's side, centred on
	/// the knob face's centre (half the width across, half the diameter down). Empty for `Chip`.
	[[nodiscard]] visage::Bounds padBounds() const;
	/// THE READING UNDER A PAD — "ON" at or above the midpoint (`on()`), else "OFF".
	[[nodiscard]] const std::string& readout() const;
	/// THE CAPTION A PAD PRINTS UNDER ITSELF — `Knob::columnCaption` at this chip's width, so a long name
	/// ends in "..." inside the column exactly where a knob's would.
	[[nodiscard]] std::string drawnCaption() const;
	/// **THE CAPTION IS A NAME** (`Knob::setCaptionIsName`, backlog OT): a pad standing in a macro's knob prints the
	/// name as typed and shrinks it to fit, as the knob does.
	void setCaptionIsName(bool isName);
	/// The size the pad's caption is drawn at (`Knob::drawnCaptionSize`).
	[[nodiscard]] float drawnCaptionSize() const;

	void draw(visage::Canvas& canvas) override;

	/// **WHAT A SWITCH IS WORTH IN A LIST ROW.** `env_card.h`'s own `kChipHeight`, so a chip seated by
	/// a generic surface and a chip drawn by a hand-written card are the same object at the same size.
	static constexpr float kHeight = 16.0f;
	/// A route drag's reach and seed — `FaderPill`'s, so a switch routes as the pill's power does.
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;

	/// Whether the switch reads as on — at or above the midpoint of its declared range, which is what a
	/// stepped 0..1 control means when a host automates it with a ramp rather than with two values.
	[[nodiscard]] bool on() const;
	/// GREYED (the honesty rule): a switch that changes nothing in the present state draws unlit and takes no press. On by default.
	void setEnabled(bool enabled);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }
	/// ENABLED HERE AND IN ITS PLACE: a control that answers `unavailable()` (a Poly switch inside a Rack) greys too.
	[[nodiscard]] bool usable() const { return enabled_ && control_.unavailable() == nullptr; }
	/// **THIS CHIP SHOWS AND TAKES MODULATION** (backlog NO item 2 follow-up: a Rack chain's mute and solo), as the
	/// `FaderPill`'s power does: armed and routable, the chip glows and a press-drag (up or right) sets the armed source's
	/// amount instead of toggling — a double-click unroutes; routed and not armed, a small ring at the right end says so,
	/// its centre filled while modulation has the switch ON. Alt presses the switch whatever is armed.
	/// **ON BY DEFAULT, HOUSE-WIDE** (backlog QK: yes to "flip the default"): every modulatable chip on
	/// every device glows iff routable, the knob's and the box's rule — twelve Pattern chips and a dozen devices' worth had
	/// simply never opted in. A chip over a control no source may reach (`display().isDestination` false) draws exactly what
	/// a plain chip draws, so the flag changes nothing there; `setShowsModulation(false)` is kept for a chip that is NOT a
	/// param control at all, and each such call says why.
	void setShowsModulation(bool shows);
	/// **PENDING** (Pattern's Go shows a pending state after it is clicked, until it fires): a still look — a doubled accent border with the caption in the accent — so it reads the same
	/// under reduced motion. Set from the DEVICE's published state, never guessed by the surface.
	void setPending(bool pending) {
		if (pending != pending_) {
			pending_ = pending;
			redraw();
		}
	}
	[[nodiscard]] bool pending() const noexcept { return pending_; }
	/// **DORMANT** (Pattern's F: a fill tab does not join the cycle when E is off): on, and doing
	/// nothing in the present state — drawn lit at half strength with a muted caption, so it reads as "set, but asleep".
	/// Still a switch: a press toggles it. Set by the surface that knows why; the tooltip says why.
	void setDormant(bool dormant) {
		if (dormant != dormant_) {
			dormant_ = dormant;
			redraw();
		}
	}
	[[nodiscard]] bool dormant() const noexcept { return dormant_; }
	[[nodiscard]] bool showsModulation() const noexcept { return showsModulation_; }
	/// **THE CONTROL'S PANEL STATE CHANGED**: re-subscribe now, off the old state's watcher list (backlog PM).
	void modulationChanged() { refreshWatch(); }
	/// WHAT THE NEXT FRAME DRAWS FOR MODULATION — the paint's own inputs, since a repaint cannot be observed headlessly.
	struct ModMarks {
		bool glow = false;       ///< armed and routable: the chip lit in the modulation colour
		float reach = 0.0f;      ///< the armed source's reach on the switch, 0..1 (the glow's depth)
		bool ring = false;       ///< routed and not armed
		bool ringFilled = false; ///< and modulation has it ON
	};
	[[nodiscard]] ModMarks modMarks() const;

	/// The caption as DECLARED — what the caller named this control. See `caption()` for what is
	/// actually drawn, which is not always this.
	[[nodiscard]] const std::string& label() const { return label_; }

	/// **WHAT THE CHIP ACTUALLY PRINTS, and it is the label only when lit/unlit can carry the value.**
	///
	/// This header used to assert, flatly, that *"a switch prints WHAT IT IS, not what it is set to:
	/// the lit state carries the value"*. That is true of `Off`/`On` and false of any other pair —
	/// the delay's `Mode` is `Clean`/`Granular`, and a lit chip captioned `Delay Mode` never names
	/// either state. So a chip over a NAMED two-way choice prints the live value instead, and the
	/// `Off`/`On` chips every other device has are untouched. The reasoning is in the .cpp.
	///
	/// Returns a reference, so it is safe to draw with and cheap to assert against.
	[[nodiscard]] const std::string& caption() const;

	/// THE CONTROL THIS CHIP DRAWS, exactly as `Knob::control()` and `ValueBox::control()` expose
	/// theirs — a sweep over "every implementation of `Modulatable`" cannot find them through one
	/// accessor otherwise.
	[[nodiscard]] device::Modulatable& control() const noexcept { return control_; }

	/// Told when a press changed the value, so a card can record one undo step. Before and after, like
	/// `ValueBox::onEdited`, and for the same reason: the widget deliberately does not know the address
	/// of the thing it edits. A MOMENTARY reports once, at the release, and only if the value it leaves
	/// differs from the one it found — a press from rest returns to rest and changes nothing.
	void onEdited(std::function<void(double before, double after)> callback) {
		edited_ = std::move(callback);
	}

	/// TEST SEAM — toggle it the way a press does, without synthesising one. A press is not observable
	/// headlessly and the VALUE is the claim.
	void toggleForTest() { toggle(); }
	/// **RE-CAPTION IT.** A macro's chip prints the macro's name, and a macro can be renamed under it
	/// (`MacroSlot`). Every other chip is captioned once, at construction.
	void setLabel(std::string label);

	/// **LET A HELD MOMENTARY GO — WRITE MIN, CLOSE THE GESTURE — as the release would.** Nothing
	/// when nothing is held. The chip does this itself when IT is hidden or destroyed; it is public for
	/// the one case the chip cannot see, which is an ANCESTOR being hidden: visage tells only the frame
	/// whose own flag moved, so an owner that is hidden must let go on the chip's behalf (`MacroSlot`).
	void cancelHold();

private:
	void toggle();
	void press();
	void release();
	void visibilityChanged() override;
	[[nodiscard]] const std::string& valueName() const;

	device::Modulatable& control_;
	std::string label_;
	bool pending_ = false;
	bool dormant_ = false;
	std::string fullName_;
	bool captionIsName_ = false;
	bool enabled_ = true;
	/// Holds the live value's name so `caption()` can hand back a reference without allocating on the
	/// draw path. Mutable because `caption()` is a const read of what is, in effect, a cache.
	mutable std::string valueCache_;
	std::function<void(double, double)> edited_;
	Behaviour behaviour_ = Behaviour::Latch;
	std::function<void()> popup_;
	std::function<bool(const visage::MouseEvent&)> pressOverride_;
	Presentation presentation_ = Presentation::Chip;
	/// THE KNOB'S DEFAULT, BY NAME: a pad stands in a knob's column, and a slot that never sets a size
	/// must get two controls of one size rather than two defaults that happened to be typed alike.
	float faceDiameter_ = Knob::kDiameter;
	// MODULATION (`setShowsModulation`): the press in flight is a route's amount, and the panel state is watched so the
	// glow repaints when the arm moves (`FaderPill`'s subscription).
	bool showsModulation_ = true; // ON BY DEFAULT house-wide (backlog QK: 2026-11-07): see `setShowsModulation`
	bool routing_ = false;
	float routeStartX_ = 0.0f;
	float routeStartY_ = 0.0f;
	float anchorAmount_ = 0.0f;
	Watch watch_;
	const ModState* watched_ = nullptr;
	void refreshWatch();
	/// A momentary press is in flight: set by the left press that opened the gesture, cleared by the
	/// release that closes it. It is what makes a release with no press behind it — or one following a
	/// right-click — write nothing and close nothing.
	bool held_ = false;
	/// The value before the held press, so the release can report the whole gesture as one edit.
	double heldFrom_ = 0.0;
};

/// **A CHIP THAT SHOWS AND TAKES MODULATION** (`SwitchChip::setShowsModulation`) — what a surface whose controls are
/// destinations builds every chip with (Pattern's Go did not glow without it). Over a control no source may
/// reach, it draws exactly what a plain chip draws.
template <class... Args>
[[nodiscard]] std::unique_ptr<SwitchChip> makeModulatedChip(Args&&... args) {
	auto chip = std::make_unique<SwitchChip>(std::forward<Args>(args)...);
	chip->setShowsModulation(true);
	return chip;
}

} // namespace feathers::ui
