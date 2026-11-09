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

#include "fx/automod/automod_derived.h"
#include "fx/automod/automod_types.h"
#include "plugin/gui_bridge.h"

#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include <visage/ui.h>

#include <vector>

// THE AUTOMODULATOR'S PICTURE — one box, two coordinate systems, and its own frame.
//
// EXTRACTED FROM `AutomodCard` BECAUSE THE FRAME THAT DRAWS IS THE FRAME THAT MUST BE TOLD. The
// strip is about to become LIVE — a phase marker that moves with the LFO — and playbook 8i states
// the rule this extraction exists to satisfy: *"a parent told to repaint is not a child told to
// repaint"*. `OwlttGraph` is the precedent and the shape is copied from it deliberately, down to
// `update()` returning whether it asked for a frame.
//
// **IT DRAWS IN ITS OWN COORDINATES.** The card decides WHERE the picture goes (`lfoStripBounds()`,
// which is card geometry and stays there); this class decides what is IN it, against
// `localBounds()`. That split is what made `OwlttGraph`'s extraction clean and it is the same split
// here.
//
// **IT DRAWS WHAT THE DSP WILL DO, RATHER THAN A SECOND IMPLEMENTATION OF IT**, which is CLAUDE.md's
// rule pointed at a picture and is the whole of why three functions moved out of `automod.cpp` to
// make it possible. The strip calls `coordsOf`, `deriveAutomod`, `evalLfoShape`, `modulationFor` and
// `cornerHzFor` — every one of them the function the audio path itself calls. There is no arithmetic
// in this file that the sound does not also run.

namespace feathers::ui {

class AutomodStrip final : public visage::Frame {
public:
	AutomodStrip(backplane::DeviceHost& host, DeviceInstanceId id);
	AutomodStrip(backplane::DeviceHost&, size_t) = delete; // a lane or a slot is not an id

	void draw(visage::Canvas& canvas) override;

	/// ADVANCE THE MARKER, AND SAY WHETHER THE SCREEN IS OUT OF DATE — the contract
	/// `OwlttGraph::update()`, `PerformStage::update` and `NotifyTray::update` already share, and it
	/// is copied from the first of those rather than reinvented.
	///
	/// IT LIVES HERE AND NOT ON THE CARD BECAUSE THIS IS THE FRAME THAT DRAWS THE MARKER. A parent
	/// told to repaint is not a child told to repaint (playbook 8i), and that exact mistake killed
	/// OWLTT's meters once already when its plot moved into its own frame.
	///
	/// **CONTENT-GATED, WHICH IS THE PROPERTY THAT MAKES THIS AFFORDABLE.** A phase that has not
	/// moved asks for nothing, so a silent plugin — no audio, or a device switched off — costs
	/// exactly zero frames. No rate throttle can offer that, and the alternative that is deliberately
	/// excluded is raising the bridge's whole-tree repaint flag from a block-rate publisher, which
	/// measured at 81% of a core and whose removal measured at -53%.
	bool update();

	/// WHICH PICTURE THE STRIP DRAWS.
	///
	/// **THE OVERLAY IS GONE AND IT WAS NOT A DRAWING PROBLEM.** The first version put the filter's
	/// CORNER over the LFO's time axis — real, computed from the audio path's own `cornerHzFor`, and
	/// unreadable. Corner over time makes no sense to most people; response against frequency does.
	/// A cutoff-over-time plot asks the reader to already know what a cutoff
	/// is and to hold a filter in their head while looking at a line that is not one. A response curve
	/// is the picture everyone has seen, and `filter_card` and `drawn_card` both draw one.
	///
	/// **AND THAT DECISION LOOKED LIKE IT KILLED THE OVERLAY.** The original design overlaid the two
	/// pictures on each other, which worked only because both shared ONE x-axis: time. A response
	/// curve's x-axis is FREQUENCY.
	///
	/// **THE ANSWER IS TO KEEP THE OVERLAY AND SPEND THE AXIS: a dual x axis in the same field.** One box,
	/// two coordinate systems — blue reads time across the bottom, amber reads log frequency across
	/// the top. **This is not exotic and it is not new here: the strip is ALREADY dual-Y**, blue in
	/// amplitude and amber in hertz, and has been since the first version. This finishes the idea
	/// rather than introducing one.
	///
	/// **THE COST IS A FALSE RELATIONSHIP AND IT IS THE ONLY REAL RISK.** Two curves whose horizontal
	/// position means different things invite a reader to see the LFO's peak sitting above some part
	/// of the response and infer "the sweep goes there" — which is meaningless, because those two x
	/// positions are different quantities. **Labelling is what has to defeat that**, so the axes are
	/// marked at OPPOSITE EDGES and each mark is drawn in its own trace's colour: the eye can always
	/// tell which scale it is reading, and neither axis is anonymous.
	enum class Picture {
		LfoOnly, ///< the waveform alone, no filter picture
		Dual,    ///< one box, two coordinate systems — the chosen picture
	};
	void setPicture(Picture which);
	[[nodiscard]] Picture picture() const noexcept { return picture_; }

	/// WHETHER THE RESPONSE SHOWS THE SWEEP.
	///
	/// A RESPONSE CURVE IS A SNAPSHOT AND THIS DEVICE'S CORNER MOVES — which is the one thing the
	/// corner-over-time trace did say and a plain curve does not. `Band` draws the response at the
	/// two ends of the LFO's travel and shades between them, so "how far does this actually sweep"
	/// survives the move to a frequency axis. That question is what found the 14.69% quiet cube.
	enum class ResponseStyle { Plain, Band };
	void setResponseStyle(ResponseStyle style);
	[[nodiscard]] ResponseStyle responseStyle() const noexcept { return responseStyle_; }

	/// WHERE THE LFO IS, 0..1 — read from the bridge, published by the DSP once per tick.
	///
	/// **A PULL, LIKE `OwlttGraph`'s METERS.** `host_.automodPhase(slot)` is a read, so a second
	/// strip on the same slot needs no plumbing and cannot fight the first for a connector.
	[[nodiscard]] float phase() const;
	/// THE ENVELOPE FOLLOWER'S LEVEL, 0..1 — reading 1, published beside the phase.
	///
	/// IT IS THE POST-TRIM, BOUNDED NUMBER the steering actually uses, not the raw follower state. A
	/// meter drawn from the raw level would be honest about a quantity the DSP then clamps, which is
	/// the more misleading of the two truths.
	[[nodiscard]] float envelope() const;

	/// WHAT THE COMB SAYS THE PHASE STAR SHOULD LOOK LIKE — its tint and how far its halo opens.
	struct CombGlow {
		uint32_t tint;  ///< the star's colour: red at the comb's low pitch, blue at its high one
		float halo;     ///< multiplies `starGlyph`'s halo, which is already three times the glyph
	};

	/// COMPUTED ONCE AND EXPOSED, rather than computed in `drawLfo` and asserted from a picture.
	/// CLAUDE.md's rule: a claim about a value cannot be inferred from a rendered buffer, and "the
	/// star is blue here and red there" is exactly such a claim. `drawLfo` calls this; so does the
	/// case. There is no second arithmetic for the two to disagree about.
	/// THE ENVELOPE TRACE'S INK AND HOW STRONGLY IT IS DRAWN.
	struct EnvTrace {
		uint32_t ink;
		float alpha;
	};

	/// COMPUTED ONCE AND EXPOSED, for `combGlow`'s reason: an opacity is a VALUE, and an opacity that
	/// is always 1.0 draws identically to a correct one at full influence — so a picture cannot tell
	/// the two apart and a case must read the number.
	[[nodiscard]] EnvTrace envTrace() const;

	/// @param lfo the LFO's value where the star sits, -1..+1 — `Marker::lfo`.
	[[nodiscard]] CombGlow combGlow(float lfo) const;

private:
	/// THE ENVELOPE'S TRACE — a level line across the same plot the waveform and the response share.
	void drawEnvelope(visage::Canvas& canvas);

public:

	/// WHERE THE MARKER IS DRAWN, in this frame's coordinates.
	///
	/// **A DRAWN POSITION IS NOT OBSERVABLE HEADLESSLY**, and this graph's neighbour has shipped
	/// three bugs of the form "drawn here, grabbable there". `draw()` calls THIS rather than
	/// computing its own, so a case comparing the marker with the waveform is comparing the marker
	/// that is actually drawn.
	struct Marker {
		visage::Point at { 0.0f, 0.0f };
		bool drawn = false;
		/// THE LFO'S VALUE WHERE THE MARK SITS, -1..+1 — carried rather than recovered from `at.y`.
		/// The star's tint is the comb's pitch AS MODULATED, and the modulation is this number; going
		/// back from a pixel would mean inverting `lfoY`'s clamp and its inset, which is two chances
		/// to disagree with the drawing about where the mark is.
		float lfo = 0.0f;
	};
	[[nodiscard]] Marker marker() const;


	/// THE PICTURE'S RECTANGLE, in this frame's own coordinates — `localBounds()`, which is what the
	/// card sized it to.
	[[nodiscard]] visage::Bounds plot() const;

	/// THE RESPONSE'S RECTANGLE — **the same box the waveform uses**, which is the whole of "same
	/// field". Empty when the picture does not include a response.
	[[nodiscard]] visage::Bounds responsePlot() const;

	/// WHAT THE RESPONSE CURVE WILL DRAW, as numbers — same reason `PreviewTrace` exists.
	struct ResponseTrace {
		std::vector<float> hz;    ///< log-spaced across the audible span
		std::vector<float> gain;  ///< linear, at the corner's resting position
		/// AT THE TWO ENDS OF THE LFO'S TRAVEL — **EMPTY UNLESS THE STYLE DRAWS THEM.** Filling them
		/// unconditionally cost 6.3 µs of this strip's 11, for two curves the shipping picture does
		/// not draw; `responseTrace`'s own note carries the measurement.
		std::vector<float> gainLo;
		std::vector<float> gainHi;
		/// WHERE THE CORNER IS RIGHT NOW, in Hz — the number the curve is drawn AROUND.
		///
		/// CARRIED RATHER THAN LEFT AS A LOCAL, because it is the quantity the defect was about:
		/// "the drawn curve does not move with the cutoff knob" is a claim about THIS, and a case that
		/// recomputed it from `cornerHzFor` would be asserting its own arithmetic rather than the
		/// strip's. See the case for why the first fix to that report passed a test and changed
		/// nothing.
		float liveHz = 0.0f;
	};
	[[nodiscard]] ResponseTrace responseTrace() const;

	/// WHAT THE STRIP WILL DRAW, AS NUMBERS.
	///
	/// **A DRAWN CURVE IS NOT OBSERVABLE HEADLESSLY AND A PICTURE OF A WAVEFORM IS THE WORST CASE OF
	/// IT** — CLAUDE.md: "a claim about a value cannot be inferred from a rendered buffer", and every
	/// instance so far has been a PARTIAL effect surviving where a total one would have been caught.
	/// A strip drawing the shape at the wrong coordinate, or with Manual dropped, still draws a
	/// perfectly plausible waveform. So the numbers are exposed and the cases assert them against an
	/// independent restatement of the audio path.
	struct PreviewTrace {
		std::vector<float> phase;    ///< 0..1 across one cycle, strictly increasing
		std::vector<float> lfo;      ///< the shape at that phase, -1..+1
		std::vector<float> cutoffHz; ///< the corner the filter will sit at, Manual and Depth folded in
	};
	[[nodiscard]] PreviewTrace previewTrace() const;

	/// THE CORNER'S TRAVEL OVER ONE LFO CYCLE, in Hz — the two ends of what `Band` shades.
	///
	/// **THIS IS WHAT SURVIVED THE AUTO-RANGE FINDING.** An earlier version drew the corner against
	/// TIME on an axis fitted to its own sweep, and the trace reproduced the waveform exactly: the
	/// corner is `base * 2^(k * lfo)` and a log axis fitted to the sweep divides out both terms,
	/// leaving `lfo`. The lesson kept its shape when the picture changed — an axis fitted to the data
	/// cannot say how much data there is — so the sweep is shown as a SPAN on an absolute frequency
	/// axis, where its width is a fact rather than a normalisation.
	struct CornerTravel {
		float lowHz;
		float highHz;
	};
	[[nodiscard]] CornerTravel cornerTravel() const;

	/// **THE SETTINGS AND THE DERIVE TOGETHER, WITH THE DERIVE GATED ON CHANGE.**
	///
	/// The card used to call `settings()` and `deriveAutomod` afresh at each of four sites, which was
	/// harmless while a card only redrew on a knob move. **It stops being harmless the moment
	/// anything redraws this strip per frame** — and that is precisely what the live-phase work would
	/// do, so the gate goes in before it rather than after.
	///
	/// GATED THE WAY THE ENGINE GATES ITS OWN: bitwise equality on the arguments, not a tolerance.
	/// `Automod::refresh()` states the reason and it applies unchanged here — a derive is a pure
	/// function of these fields, so equal fields mean an equal answer and there is nothing to
	/// invalidate.
	///
	/// **THE CHAIN READ IS NOT GATED AND CANNOT BE**: reading the row is how the strip finds out
	/// whether anything moved, so it happens on every call by construction. What the gate saves is
	/// the derive — MEASURED at 0.309 µs against 3.431 µs for the magnitude evaluation, so 9% — and
	/// what computing this ONCE per frame saves is three of the four chain copies.
	struct View {
		fx::automod::Settings settings;
		fx::automod::AutomodDerived derived;
	};
	[[nodiscard]] const View& view() const;

	/// HOW MANY TIMES THE DERIVE HAS ACTUALLY RUN.
	///
	/// **THE GATE IS A PERFORMANCE PROPERTY AND WOULD OTHERWISE BE UNASSERTABLE** — a cached derive
	/// and an ungated one return the same numbers, so no comparison of outputs can tell them apart.
	/// A count produced BY the derive is the only thing that cannot be had without one, which is the
	/// same argument `Gui::Impl::tick()` makes for having `notify()` return how many frames it told.
	[[nodiscard]] size_t deriveCountForTest() const noexcept { return deriveCount_; }

	/// THE DEVICE'S SETTINGS AS THE STRIP READS THEM — declared defaults under whatever the row
	/// carries. Public because a case that checks the picture has to be able to say what it is a
	/// picture OF.
	[[nodiscard]] fx::automod::Settings settings() const;

private:
	/// THE MARKER, GIVEN A TRACE ALREADY IN HAND. `draw` has one; `marker()` builds one. **The split
	/// is a measurement rather than tidiness**: this is now a per-frame path, and having `draw` call
	/// the public form would walk `previewTrace()` twice for every frame the star moves.
	[[nodiscard]] Marker markerOn(const PreviewTrace& trace) const;

	void drawLfo(visage::Canvas& canvas);
	void drawResponse(visage::Canvas& canvas);
	void drawAxes(visage::Canvas& canvas);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	/// THE CANDIDATE UNDER REVIEW. Both pictures and both response styles are runtime-switchable so
	/// `widget-shot` can photograph every combination without a rebuild — the choice is made from pictures,
	/// which is how the first version was settled and how the auto-range was caught.
	Picture picture_ = Picture::Dual;

	/// **PLAIN SHIPS.** The range was rejected — the intent was an instantaneous curve — so the
	/// resting corner is what the card draws. `Band` is kept, reachable only from `widget-shot`, for
	/// one stated reason: the instantaneous curve is SEQUENCED next rather than cancelled, and the
	/// column-tiling it proved is the technique any filled element on this strip will want. It is not
	/// a control and no surface offers it.
	ResponseStyle responseStyle_ = ResponseStyle::Plain;

	/// THE GATE'S MEMORY. `mutable` because `view()` is const and a cache is not a change to what the
	/// strip IS.
	/// WHAT PHASE WAS LAST DRAWN, so `update()` can tell a moving LFO from a still one. The default
	/// matches what a fresh bridge answers, which is what a freshly built strip has drawn.
	float drawnPhase_ = 0.0f;
	float drawnEnvelope_ = 0.0f;
	/// THE SHAPE ON SCREEN, beside the phase. Watching the phase alone left the curve stale while the
	/// star moved over it — see `update()`.
	fx::automod::Settings drawnShape_ {};
	bool haveDrawnShape_ = false;

	mutable View view_ {};
	mutable bool viewValid_ = false;
	mutable size_t deriveCount_ = 0;
};

} // namespace feathers::ui
