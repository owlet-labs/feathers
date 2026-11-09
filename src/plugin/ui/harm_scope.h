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

#include "fx/harm/harm_types.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/control_face.h"

#include "backplane/device_host.h"

#include <visage/ui.h>

#include <array>
#include <cstddef>
#include <memory>

// HARM, DRAWN — the notch and the harmonic it is coupled to, on one axis.
//
// THE AXIS IS RATIO, NOT HERTZ, and that is the whole reason this picture can exist. `harm_card.h`
// recorded the objection: *"the notch's frequency depends on the NOTE BEING PLAYED, and a card has
// no note. Drawing it against a pretend middle C would be a curve that is wrong for every other
// note."* True, and the same paragraph's next sentence is the answer — *"what the card shows instead
// is the RATIO, which is the part that is actually a setting: '3:2' is true at every pitch."* The
// notch centre is the note TIMES a ratio, so on a ratio axis it sits at the selected harmonic for
// every note, and the picture is honest at all of them rather than at one.
//
// SO THIS WIDGET NEVER ASKS WHAT NOTE IS PLAYING, and there is a case asserting that changing the
// note moves nothing on it. That is not an optimisation; it is the claim the axis is built on.
//
// THE Y AXIS IS LINEAR AMPLITUDE, NOT dB, which is the one place this deliberately differs from the
// filter's curve. A notch's floor is minus infinity and a dB axis has to invent a bottom for it;
// worse, Depth's promise is stated linearly — `|H| = 1 - depth` exactly — so a linear axis is the
// one on which a half-depth notch is drawn half way down. The filter has resonant PEAKS to show and
// needs the compression of dB; this has one trough whose depth is a number the user set.

namespace feathers::ui {

class HarmScope : public visage::Frame, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	HarmScope(backplane::DeviceHost& host, DeviceInstanceId id);
	HarmScope(backplane::DeviceHost&, size_t) = delete; // a lane or a slot is not an id

	void draw(visage::Canvas& canvas) override;

	/// WHICH DEVICE'S HARM THIS DRAWS, BY ID. A scope built for one device and shown over another would
	/// be the picture lying about which device it is.
	DeviceInstanceId device() const noexcept { return device_; }

	//-----------//
	// The curve //
	//-----------//

	/// Points per LOGICAL pixel, times the display scale — WaveDisplay's convention and the filter
	/// card's, because there is one right answer to "how many points is a drawn curve" here.
	///
	/// IT IS NOT LOAD-BEARING THE WAY THE FILTER'S IS, and that is worth saying so nobody raises it
	/// hoping for a deeper notch. A true null is infinitely narrow and no grid finds it; the trough
	/// is exact because `ratioProfile` PLACES a point on the centre, at any count. See the case, and
	/// the measured table in harm.cpp.
	static constexpr float kPointsPerPixel = 2.0f;
	static constexpr size_t kMinCurvePoints = 257;
	static constexpr size_t kMaxCurvePoints = 2048;

	size_t curvePointCount() const;

	/// The curve this widget would draw, in LINEAR amplitude. Public for the reason the filter card's
	/// is: a case should read what reached the glass rather than recompute it.
	size_t curveGains(float* out, size_t capacity) const;

	//---------------------//
	// Where things land   //
	//---------------------//

	/// The box the curve is drawn in, in this frame's coordinates.
	visage::Bounds plotBounds() const;

	/// Where a ratio sits on the x axis — log-spaced across the harmonic table's own ends.
	/// Outside `[kProfileLoRatio, kProfileHiRatio]` this still extrapolates rather than clamping,
	/// because a caller that wanted a clamp can clamp and one that wanted the truth cannot unclamp.
	float xForRatio(double ratio) const;

	/// Where the NOTCH is drawn — `xForRatio(notchRatio(settings()))`, and therefore following the
	/// level<=0 fallback and the fine detune rather than the harmonic control alone.
	float notchX() const;

	/// Where the sub-oscillator's harmonic is drawn, and how tall its stem is as a fraction of the
	/// plot — the Level control, which is the only thing on this picture that is not the notch.
	float oscillatorX() const;
	float oscillatorStem() const;

	//----------------------------------------------//
	// THE PICTURE IS THE CONTROL — grips on the scope //
	//----------------------------------------------//
	//
	// The expanded view should be more intuitive than a knob list. The
	// grouping was half the answer; this is the other half. HARM is unusual in that **the picture
	// already draws what five of its controls do** — so a knob grid underneath asks the player to
	// hold a mapping in their head between a row of circles and a picture already showing them the
	// answer.
	//
	// THE KNOBS STAY. The scope is the fast way and the knob is the precise one, and a value you can
	// only set by dragging a curve is worse than one you can also type.
	//
	// EVERY GRIP GOES THROUGH THE SAME `Modulatable` THE KNOBS USE, and that is not a tidiness
	// preference — it is the constraint that makes them controls at all. A drag that wrote a value
	// directly would give a thing that cannot be armed, cannot glow, cannot be a modulation
	// destination and does not undo. This repo shipped exactly that failure today by a different
	// route: HARM's adapters never learned the modulation state, and ten declared destinations were
	// unreachable while every widget looked perfect.
	enum class Grip { None, Notch, Trough, Stem, Shoulder };

	/// WHETHER THE PICTURE IS A CONTROL. **OFF, decided after hands-on use:** the plot-based edits were
	/// confusing, so they are disabled; the knobs edit, and the graph is display only.
	///
	/// DISABLED RATHER THAN DELETED, which is the right shape
	/// regardless: the gesture road is the part that took the work and would take it again, and
	/// nothing about it was found wrong. What it bought stands whatever happens to it: four handle
	/// collisions the sweep found, the day's fifth render-only defect, and the empty-gesture gap that
	/// became backlog 18.
	///
	/// WHAT IS OFF IS THE HIT TEST AND THE HANDLES. The scope draws exactly what it drew before —
	/// curve, notch, stem, harmonic position — because all of that is the DISPLAY the brief asked for
	/// and none of it is what was confusing.
	///
	/// THE CASES KEEP DRIVING IT, which is why this is a switch and not a `#if 0`: a road nothing
	/// exercises rots, and the day it comes back it would come back untested. They turn it on
	/// explicitly and say that they are doing so.
	void setGripsEnabled(bool enabled);
	bool gripsEnabled() const noexcept { return gripsEnabled_; }

	/// WHICH GRIP A POINT IS ON, in this frame's coordinates. Ordered smallest-target-first, the rule
	/// every hit test in this editor follows: a big region tested before a small one inside it makes
	/// the small one unreachable, and the bug reads as "the control does nothing".
	Grip gripAt(visage::Point point) const;

	/// The rectangle a grip occupies — drawn AND hit-tested from this one answer, so a handle cannot
	/// be drawn somewhere it cannot be pressed. That is not hypothetical: a panel of perfect widgets
	/// shipped this morning with no hit region at all.
	visage::Bounds gripBounds(Grip grip) const;

	/// Where the notch has recovered to about -3 dB — the shoulder, found by walking the curve. It is
	/// the handle's home and it moves with Width by construction.
	float shoulderX() const;

	/// WHICH CONTROL A GRIP EDITS. `Param::kCount` for `Grip::None`.
	static fx::harm::Param paramOf(Grip grip);

	/// Hand the scope the modulation state, exactly as a card hands it to a knob — and to EVERY
	/// ADAPTER, not just the ones a widget holds. See `HarmCard::setModulation` for what the other
	/// spelling cost.
	void setModulation(ModState* mod, EditHistory* history);

	/// Whether a grip would write a ROUTE AMOUNT rather than a value — i.e. a source is armed and may
	/// reach this control. `Knob::mappable`'s question, asked of the same `display().routable`.
	bool gripMappable(Grip grip) const;

	//-----------//
	// For tests //
	//-----------//

	/// THE SETTINGS THE PICTURE IS DRAWN FROM. Exposed for the reason FilterCard exposes its own: a
	/// promise about an assembly needs the assembly readable, and the only check that catches a
	/// control the picture forgot is comparing it field for field against the device's own state.
	fx::harm::Settings settingsForTest() const { return settings(); }

	/// The grip a drag is currently on, so a case can prove a press took hold rather than that a
	/// value happened to move.
	Grip heldForTest() const noexcept { return held_; }

private:
	fx::harm::Settings settings() const;
	DeviceControl& control(fx::harm::Param param) const;
	void commit(fx::harm::Param param, double value);
	/// What a drag at `at` means for `grip`, in the control's own declared units.
	double valueForDrag(Grip grip, visage::Point at, bool fine) const;
	/// The table index whose ratio sits at `x`, snapped. Shared by the rail and the stem, which now
	/// both drag the harmonic.
	double harmonicForX(float x) const;
	void endGesture();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	bool gripsEnabled_ = false;
	Grip held_ = Grip::None;
	bool routeDrag_ = false;
	/// PER CONTROL, because the stem's drag writes TWO of them. One flag would have closed the
	/// gesture on whichever was committed last and left the other open.
	mutable std::array<bool, static_cast<size_t>(fx::harm::Param::kCount)> gestureOpen_ {};
	visage::Point anchor_ {};
	double anchorValue_ = 0.0;
	/// How far the notch was from the pointer when the grip was taken — so a horizontal drag moves it
	/// BY the hand rather than TO the hand.
	float anchorNotchDx_ = 0.0f;
};

} // namespace feathers::ui
