/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <visage/ui.h>

// **THE TRIG PAD — A TRIGGER DESTINATION IS NOT A KNOB** (backlog MV addendum): a trigger destination gets its own
// UI instead of a knob, so the right mapping amount is set automatically and it takes less room than a knob. Decided
// the same day: NOT in the card header; each card
// seats it near the shape it fires; and it has ITS OWN LOOK — a round jack/LED, amber when lit, one glyph — so it
// cannot be read as an enable square, a chip or a knob.
//
// WHAT IT DOES, all through the device's ordinary Trig control (`mod/trig.h`):
//   - A PRESS FIRES BY HAND: the base is held at 1 for the press and back at 0 on release, which the edge detector
//     sees rise once. A held press fires once. It records no undo step (the value ends where it began).
//   - WITH A SOURCE ARMED, A PRESS ROUTES IT AT DEPTH 1 — the mapping a trigger wants, set for you: a one-tick
//     pulse to 1 crosses the fire level, and a 0..1 source fires at its midpoint. A second click unroutes it (the
//     knob's double-click). Alt bypasses the arm and fires by hand, the house's modifier.
//   - IT LIGHTS ON EVERY FIRE, from the device's fire count (`DeviceHost::trigFiresAt`), for `kFlashSeconds` — a
//     count, so a one-tick fire is seen by a card that looks 60 times a second.
//   - A RIGHT-CLICK LISTS THE ROUTES INTO IT (backlog MV, the open item): the same menu as any destination's.
//   - AND, ON A DEVICE WHOSE NOTES FIRE IT TOO, IT CARRIES THE NORMAL (backlog NU): the right-click's last row,
//     "Notes fire Trig", checked while a note-on fires this Trig as though patched into it; unchecked, only the Trig
//     does. A small dot over the glyph shows the normal is made. See `mod::notesTrigParamDesc` for what it does per mode.

namespace feathers::ui {

class TrigPad final : public visage::Frame, public TooltipSource {
public:
	/// THE FIRE COUNT, asked each frame — false when nobody publishes one (the pad then lights only under a hand).
	using FireCount = std::function<bool(uint32_t& fires)>;

	TrigPad(device::Modulatable& control, FireCount fires);
	~TrigPad() override;

	/// **THE NORMAL INTO THIS TRIG** (backlog NU): the device control that says whether notes fire it, and the two values
	/// that mean made and broken — 1 and 0 for a switch. A pad with none (Random Trigger's, the Advance) has no row.
	struct Normal {
		device::Modulatable* control = nullptr;
		double made = 1.0;
		double broken = 0.0;
	};
	void setNormal(Normal normal) {
		normal_ = normal;
		normalWas_ = normalled();
		redraw();
	}

	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override {
		out.push_back({ &control_, Road::Face });
		if (normal_.control != nullptr) {
			out.push_back({ normal_.control, Road::Face });
		}
	}

	/// WHAT `draw` LIGHTS (backlog QK, the glow-honesty sweep): the jack's ring in the mapping colour while the armed source
	/// may reach it, and the amber routed ring — for a route from ANY source (`routedElsewhere` too).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override {
		if (&c != &control_) {
			return {};
		}
		const device::ModDisplay d = control_.display();
		return { d.isDestination, d.isDestination && d.routable, routedNow() };
	}
	/// A route reaches this pad, from the focused source or another one.
	[[nodiscard]] bool routedNow() const {
		const device::ModDisplay d = control_.display();
		return d.isDestination && (d.amount != 0.0f || d.routedElsewhere);
	}

	void draw(visage::Canvas& canvas) override;

	/// ONE FRAME'S LOOK AT THE FIRE COUNT, at `nowSeconds` on any steady clock; true when the light changed (redrawn).
	bool poll(double nowSeconds);
	/// The same on the steady clock — what a card's per-frame update calls.
	bool poll();

	/// Whether it is drawn lit: a fire within `kFlashSeconds`, or a hand on it.
	[[nodiscard]] bool lit() const noexcept { return held_ || flashing_; }
	/// Whether a source is armed that this pad would route — drawn as the mapping ring.
	[[nodiscard]] bool mappable() const;

	[[nodiscard]] device::Modulatable& control() const noexcept { return control_; }

	/// A press and its release, as the hand makes them — `altDown` bypasses the arm.
	void press(bool altDown = false, int clicks = 1);
	/// THE RIGHT-CLICK: the routes into this Trig, each pick arming its source (so the second click of a pair then
	/// unroutes THAT one), and one row removing them all — `ui::appendRouteRows`, the rows every destination lists —
	/// then, with a normal, the checkable "Notes fire Trig". False, and nothing shown, when it would be empty.
	bool showRouteMenu();
	/// The menu `showRouteMenu` shows, built and not shown: a popup cannot be clicked from a case, so its rows and its
	/// pick are read from this. Empty when there is nothing to list.
	[[nodiscard]] visage::PopupMenu menuForTest();
	/// The normal row's id in that menu, far past any route row.
	static constexpr int kNormalRow = 3000;

	/// WHETHER THE NORMAL IS OFFERED: a normal control whose value is one of its two (S&H's Clock is a normal only
	/// under Note or Trig), and WHETHER IT IS MADE.
	[[nodiscard]] bool hasNormal() const;
	[[nodiscard]] bool normalled() const;
	/// THE ROW'S VERB: make or break the normal, one undo step through the control's own gesture. Nothing without one.
	void setNormalled(bool made);
	void release();
	/// A press that never got its release (the pad hidden, the card gone) lets go of the control.
	void cancelHold();

	static constexpr float kDiameter = 14.0f;
	static constexpr double kFlashSeconds = 0.12;
	/// AMBER, the trigger's own colour: no token in the mock is one, and the accent (periwinkle) is the enable's
	/// and the mapping's, which is exactly what this must not look like.
	static constexpr uint32_t kLitColour = 0xffF2A93B;

private:
	void visibilityChanged() override;

	device::Modulatable& control_;
	Normal normal_ {};
	bool normalWas_ = false;
	/// A popup still open when the pad is destroyed picks nothing: its row is guarded by this token.
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
	FireCount fires_;
	bool haveSeen_ = false;
	uint32_t seen_ = 0;
	bool flashing_ = false;
	double flashUntil_ = 0.0;
	double lastNow_ = 0.0;
	bool held_ = false;
	bool routing_ = false;
};

} // namespace feathers::ui
