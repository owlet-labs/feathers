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

#include "plugin/ui/automod_strip.h"

#include "fx/automod/automod.h"
#include "fx/filter/filter_types.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trace_marker.h"
#include "plugin/ui/zone_coord_cell.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace amd = feathers::fx::automod;
/// THE AUDIBLE SPAN IS BORROWED FROM THE MULTIMODE FILTER'S PROFILE — 20 Hz to 20 kHz — so a player
/// comparing this card's curve with the filter card's is comparing them on one axis.
namespace flt = feathers::fx::filter;

namespace feathers::ui {

namespace {

/// HOW FAR THE TRACES STAY OFF THE STRIP'S EDGE, so a waveform at full travel reads as full travel
/// rather than as a line pressed against the border it might be clipped by.
constexpr float kStripInset = 4.0f;

/// HOW FINELY EACH OF THE FIVE RUNS IS SUBDIVIDED, in pixels per sample.
///
/// **THE CORNERS ARE VERTICES NO MATTER WHAT THIS IS**, which is the half that matters — see
/// `previewTrace`. This only decides how densely the straight parts are checked, and a straight run
/// needs no density at all for the LFO trace. It is here for the CUTOFF trace, which is exponential
/// in the LFO and therefore curved between the very same corners.
///
/// **AND IT HAS A FLOOR THAT IS A RENDERING FACT RATHER THAN A TASTE, MEASURED IN A PHOTOGRAPH.**
/// The first version used 3, and the shot came back with both traces DASHED across every shallow
/// stretch and a grey wedge filled in between them. `canvas.h`'s `addSegment` is why: a segment's
/// quad is `[min - thickness, max + thickness]` in both axes and the endpoints are normalised into
/// it, so a nearly HORIZONTAL segment gets a box only `2 * thickness` tall — 6px here — and its
/// direction vector collapses toward the degenerate. A 3px step against a 2px weight is a dot, not a
/// line. Ten is comfortably longer than the box is tall at every slope the strip can draw, and the
/// artefact is gone from the photograph.
///
/// STEEP SEGMENTS NEVER SHOWED IT, which is what made it look like a data problem rather than a
/// drawing one: the LFO-only variant was clean because its runs are steep, and only the cutoff trace
/// — which is the same shape flattened by a log axis — was shallow enough to trip it.
constexpr float kStripPixelsPerSample = 10.0f;

/// THE RESPONSE'S OWN DENSITY, SEPARATE FROM THE WAVEFORM'S, and under test — see the note above on
/// why the waveform's floor is 10.
constexpr float kResponsePixelsPerSample = 4.0f;

/// THE RESPONSE'S VERTICAL RANGE, in decibels, **MEASURED FROM THIS FILTER RATHER THAN BORROWED.**
///
/// The first version took `filter_card.cpp`'s +30 / -42, on the reasonable-sounding argument that two
/// cards should read at the same slope. MEASURED over a 21x21 sweep of Type and Flavour, this filter
/// does not use that range: its response tops out at a **median of 0.0 dB, +7.1 at p95, +9.6 at its
/// worst**, so thirty decibels of headroom were drawing nothing, and the curve was squeezed into the
/// bottom half of its own box. Its floor does earn the room — median -23.1, p05 -45.8.
///
/// **THE REASON IT IS FLATTER THAN A FILTER CARD'S IS STRUCTURAL, NOT A DEFAULT.** This device blends
/// all three SVF outputs at once — `low*LP + band*BP + high*HP` — so it goes from `low` at DC to
/// `high` at Nyquist with a bump between. A lowpass falls off a cliff; a three-band blend does not,
/// and at the declared defaults (low 0.428, band 0.904, high 0.000) the whole response spans about
/// 11 dB. That is the honest picture of this filter, not a drawing fault.
///
/// **AND THIS IS NOT THE AUTO-RANGE TRAP** — see `Picture`'s note and playbook 8q. The range is fitted
/// to what the INSTRUMENT can reach, measured once across the whole coordinate cube, and it does not
/// move as a knob does. An axis fitted to the current sample is what reproduces its own input; an
/// axis fitted to the instrument is what lets two samples be compared.
constexpr float kResponseTopDb = 12.0f;
constexpr float kResponseBottomDb = -48.0f;

/// The four corner dots' radius, and the weight of the two traces.
constexpr float kBreakpointRadius = 2.5f;
constexpr float kTraceWeight = 2.0f;

/// THE COMB'S COLOUR, AND HOW WIDE ITS GLOW IS — the phase star glows with a tint from the comb's
/// frequency (red low, blue high) and a glow radius from the comb's intensity.
///
/// TINTED FROM THE BASE DELAY AND NOT THE MODULATED ONE, which is a choice worth stating. The comb's
/// pitch MOVES with the LFO — `Comb::delayFor` adds `modRangeSamples * lfo` — so a tint could follow
/// the sweep. It does not, for two reasons: "comb frequency" reads as a property of the POSITION
/// rather than of the instant, and the base delay is a pure function of the settings, which the strip
/// already caches and already redraws for. Following the sweep would mean restating the marker's own
/// interpolation of the trace, and this file's standing rule is not to compute a second time what
/// something else already decided. It is a small change if the live pitch turns out to read better.
///
/// LOG-NORMALISED, because pitch is. The declared span is `kCombMinDelaySamples` to
/// `kCombMaxDelaySamples` — 6 to 1534 samples, which is more than eight octaves — and a linear
/// normalisation would spend nearly all its travel in the bottom octave.
///
/// **AND BOTH CHANNELS FALL BACK WHEN THE COMB IS INAUDIBLE.** `comb.mix` is how much comb is ADDED,
/// so at zero there is no comb to have a frequency, and a star tinted for a pitch nothing is playing
/// would be the honesty rule's exact complaint — a surface reporting a mechanism the sound is not
/// running. Below the threshold the mark is the ordinary accent star it has always been.
/// THE ENVELOPE TRACE'S OWN INK — green, and chosen by measuring rather than by taste.
///
/// **THE STRIP'S PALETTE HAS A HOLE IN IT AND THE TRACE GOES THERE.** Measured as hue angles: the
/// response curve is 26 degrees and the star's low end 9, the LFO waveform 217 and the star's high
/// end 214, with `kMod` — the semantically obvious choice — at 229 and therefore useless here. Two
/// clusters, warm and cool, and nothing between them. This sits at 142: 71 degrees from the nearest
/// occupant and roughly equidistant from BOTH, which is the property that matters, because THE STAR
/// MOVES. A hue that merely differed from today's star would collide at one end of its sweep.
///
/// AND IT BELONGS TO THE FAMILY RATHER THAN SHOUTING OVER IT. Saturation 0.37 and value 0.84 against
/// `kAccent`'s 0.45/0.91 and `kValue`'s 0.47/0.90 — the same muted register the rest of the surface
/// is drawn in. A saturated green would have been distinct and wrong.
constexpr uint32_t kEnvTraceInk = 0xff86d6a4;

/// HOW FAINT THE TRACE IS WHEN THE ENVELOPE IS STEERING NOTHING — and it is NOT zero, which is the
/// second half of a decision made in two steps.
///
/// **DO NOT REMOVE THIS AS REDUNDANT.** First the trace's opacity followed the envelope's influence, and the trace
/// was made to fade out entirely when the envelope was not steering — which is honest, and which made
/// "the envelope is idle" INDISTINGUISHABLE FROM "this build has no envelope trace". You cannot tell
/// those apart by looking, and that is the whole problem. Hence a faint floor for the envelope trace.
/// The floor is what makes absence readable as rest.
///
/// 0.18 MEASURED AGAINST WHAT IS ALREADY ON THIS STRIP, the same way the ink was chosen. The faintest
/// thing drawn here is the plate's own fill at 0.12, and the curves and marks live at 0.75 to 0.90.
/// A floor at or below 0.12 would read as part of the plate rather than as a line; 0.18 is 1.5 times
/// the plate — visible as ink — and under a quarter of the curves, so it cannot compete with the LFO,
/// the response, or the star sweeping past it.
constexpr float kEnvTraceFloor = 0.18f;

/// WHERE THE TRACE REACHES WHEN THE ENVELOPE IS STEERING FULLY. Unchanged by the floor: the floor
/// lifts the bottom of the range and leaves the top where it was.
constexpr float kEnvTraceCeiling = 0.9f;

constexpr float kCombAudibleMix = 0.02f;
constexpr uint32_t kCombLowTint = 0xffe0553c;  ///< long delay, low pitch — red
constexpr uint32_t kCombHighTint = 0xff4c9bff; ///< short delay, high pitch — blue

/// HOW FAR THE GLOW OPENS between an inaudible comb and a full one. The halo is three times the
/// glyph before this multiplies it, so 2.4 is a halo a little over seven times the star — visible as
/// a bloom rather than as a second object.
constexpr float kCombGlowMin = 0.55f;
constexpr float kCombGlowMax = 2.4f;

/// HOW BIG THE PHASE MARKER'S GLYPH IS. `starGlyph` draws a halo three times this and arms nearly
/// twice it, so the mark's real footprint is about 3.5x the number — which is why 3 rather than the
/// 5 the perform stage uses: at 5 the halo is 15px inside a 62px strip and swallows the waveform it
/// is meant to point at.
constexpr float kMarkerSize = 3.0f;

} // namespace

AutomodStrip::AutomodStrip(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {}

void AutomodStrip::setPicture(Picture which) {
	if (picture_ == which) {
		return;
	}
	picture_ = which;
	// INK ONLY. Both pictures share one box, so nothing below them moves — which is the layout
	// argument for the dual axis stated as code.
	redraw();
}

void AutomodStrip::setResponseStyle(ResponseStyle style) {
	if (responseStyle_ == style) {
		return;
	}
	responseStyle_ = style;
	redraw();
}

float AutomodStrip::phase() const {
	// READING 0 — automod's one published value. `DeviceHost::readingAt` carries floats it does not
	// understand and this is the end that knows what index 0 means; `BridgeDeviceHost` is the other.
	// The mechanism was built for OWLTT's nine meters and automod cost one entry rather than a new
	// capability, which is the argument for it carrying anonymous numbers.
	return host_.readingCountAt(device_) > 0 ? host_.readingAt(device_, 0) : 0.0f;
}

AutomodStrip::EnvTrace AutomodStrip::envTrace() const {
	// **THE OPACITY IS WHAT THE ENVELOPE ACTUALLY STEERS, NOT HOW MUCH IT COULD.** `envelopeSteerFor`
	// (automod.h) scales the depth by `depth * envDepthInfluence * min(level, 1)`, so the amount by
	// which this envelope moves the modulation depth is that PRODUCT, relative to the depth it moves.
	// An earlier version used the influence alone, which reads as "how much the field says this
	// matters" and leaves a trace fully lit while sitting at rest, steering nothing. A picture claiming
	// a mechanism the sound is not running is the honesty rule's own complaint.
	//
	// WHAT THIS DOES NOT DRAW (parity port, 2026-09-16): the envelope's DERIVATIVE share of the same
	// scale, and its VALUE road into the LFO. Neither is published — the readings carry phase and level
	// — so the trace shows the level road and under-reports an attack. Said here rather than faked.
	//
	// BOUNDED THE SAME WAY THE SOUND BOUNDS IT. The level is clamped at 1 before it steers — that clamp
	// is what stops a hot bus inverting the LFO — so reading the raw follower here would report a
	// steering the DSP refuses to apply.
	const float level = std::min(envelope(), 1.0f);
	const float steering = std::clamp(std::fabs(level * view().derived.envDepthInfluence), 0.0f, 1.0f);
	// FLOORED, NOT SCALED FROM ZERO — see `kEnvTraceFloor`. The range runs from "idle and visible" to
	// the ceiling the trace already had.
	return {kEnvTraceInk,
	        kEnvTraceFloor + (kEnvTraceCeiling - kEnvTraceFloor) * steering};
}

AutomodStrip::CombGlow AutomodStrip::combGlow(float lfo) const {
	// SEE `kCombLowTint` for why the normalisation is logarithmic and why an inaudible comb gets no
	// colour of its own.
	const amd::CombSettings& comb = view().derived.comb;
	const float mix = std::clamp(comb.mix, 0.0f, 1.0f);
	if (mix <= kCombAudibleMix) {
		return {kAccent, 1.0f};
	}
	const float lo = std::log(amd::kCombMinDelaySamples);
	const float hi = std::log(amd::kCombMaxDelaySamples);
	// AS MODULATED, THROUGH THE DSP'S OWN FUNCTION. `combDelayFor` is what `Comb::delayFor` calls, so
	// the pitch this tint reports is the pitch the comb is running — including the clamp at both ends
	// of the line, which is audible as the comb's pitch flattening and must therefore be visible too.
	const float here = std::log(amd::combDelayFor(comb, lfo));
	// 0 AT THE LONGEST DELAY, 1 AT THE SHORTEST — pitch runs the opposite way to delay, which is the
	// whole reason this subtraction is the way round it is.
	const float highness = std::clamp((hi - here) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
	return {blendColour(kCombLowTint, kCombHighTint, highness),
	        kCombGlowMin + (kCombGlowMax - kCombGlowMin) * mix};
}

float AutomodStrip::envelope() const {
	// READING 1. GUARDED ON `> 1` AND NOT ON `> 0`, which is the whole reason this is a separate
	// function rather than a second index at the call site: a host that publishes only the phase —
	// any build before the envelope was published, and `BridgeDeviceHost` if it is ever asked for a
	// slot that has not ticked — answers a count of 1, and `readingAt(slot, 1)` on it is a read past
	// the end of what was published.
	return host_.readingCountAt(device_) > 1 ? host_.readingAt(device_, 1) : 0.0f;
}

bool AutomodStrip::update() {
	// **COPIED FROM `OwlttGraph::update()`, WHICH IS THE SHIPPED AND MEASURED VERSION OF THIS.** One
	// read, one comparison against what was drawn, and a `redraw()` only when they differ.
	//
	// EXACT EQUALITY RATHER THAN A TOLERANCE, deliberately: the question is "is the screen showing
	// this number", which is a bitwise question. A tolerance here would be a rate limiter wearing a
	// correctness argument, and it would stall the marker on a slow LFO — 0.1 Hz moves the phase by
	// 1.3e-4 per tick, which any plausible epsilon would swallow entirely.
	// **THREE INPUTS, AND THE REDRAW IS THEIR UNION** — the phase, the settings, and the envelope.
	// Two arcs widened this gate independently and both were fixing the same class of defect: a
	// surface correctly reporting that nothing IT LOOKED AT had changed while something else moved.
	//
	// THE SETTINGS, because watching the phase alone left the star following the LFO over a curve
	// that never moved. The defect: moving the cutoff knob did not move the drawn curve, though the XY
	// pad and the actual modulation both did. That difference is the whole diagnosis —
	// a pad drag repaints the card as a side effect of the gesture and modulation moves the phase
	// this already watched, so two of the three inputs happened to work and a knob, which repaints
	// only itself, did not.
	//
	// THE ENVELOPE, because it moves when the phase does not — a held note under a stopped LFO is
	// exactly that case, and the trace would have sat frozen while the follower worked.
	//
	// EXACT EQUALITY ON ALL THREE: the question is "is the screen showing this", which is bitwise.
	const float now = phase();
	const float env = envelope();
	const amd::Settings shape = settings();
	if (now == drawnPhase_ && env == drawnEnvelope_ && haveDrawnShape_ && shape == drawnShape_) {
		return false;
	}
	drawnPhase_ = now;
	drawnEnvelope_ = env;
	drawnShape_ = shape;
	haveDrawnShape_ = true;
	redraw();
	return true;
}

visage::Bounds AutomodStrip::plot() const { return { 0.0f, 0.0f, width(), height() }; }

visage::Bounds AutomodStrip::responsePlot() const {
	// THE SAME BOX. "Same field" is the design, so there is no second rectangle to keep in step with
	// the first — the two pictures share one, and what separates them is their axes rather than their
	// geometry.
	return (picture_ == Picture::LfoOnly) ? visage::Bounds {} : plot();
}

const AutomodStrip::View& AutomodStrip::view() const {
	const amd::Settings now = settings();
	if (!viewValid_ || !(now == view_.settings)) {
		view_.settings = now;
		view_.derived = amd::deriveAutomod(amd::coordsOf(now, amd::nominalRateHz(now)));
		viewValid_ = true;
		++deriveCount_;
	}
	return view_;
}

amd::Settings AutomodStrip::settings() const {
	// THE DECLARED DEFAULTS FIRST, THEN WHATEVER THE ROW ACTUALLY CARRIES, which is the road
	// `device_row.h` names in its own words: *"Fields past the returned count are LEFT UNTOUCHED
	// rather than zeroed, so a caller may pre-fill `out` with defaults if it prefers that road."*
	//
	// IT MATTERS HERE MORE THAN MOST. This device's defaults are 0.30 / 0.70 / 0.10, chosen by
	// measurement — a zero-filled short row would draw the waveform at the ORIGIN, which is the one
	// coordinate measured to have 0.000 octaves of travel in it. The picture would be a flat line for
	// a device that is sweeping.
	// THE ROW STARTS AT THE DEVICE'S DEFAULTS AND IS OVERWRITTEN AS FAR AS THE HOST HAS FIELDS, which
	// is the point of the comment above: a zero-filled short row would draw the origin, and the origin
	// is the one coordinate with no travel in it.
	amd::Row row = amd::rowOf(amd::Settings {});
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return amd::settingsOf(row);
}

AutomodStrip::CornerTravel AutomodStrip::cornerTravel() const {
	// FROM THE SAME TRACE THE WAVEFORM IS DRAWN FROM, so the shaded span and the wave beside it are
	// statements about one cycle rather than two computations that happen to agree.
	const PreviewTrace trace = previewTrace();
	if (trace.cutoffHz.empty()) {
		return { amd::kFilterMinHz, amd::kFilterMinHz };
	}
	const auto [lo, hi] = std::minmax_element(trace.cutoffHz.begin(), trace.cutoffHz.end());
	return { *lo, *hi };
}

AutomodStrip::ResponseTrace AutomodStrip::responseTrace() const {
	const View& v = view();
	const amd::Settings& s = v.settings;
	const amd::AutomodDerived& d = v.derived;
	const double rate = host_.sampleRate();

	// **THE CURVE IS DRAWN AT THE CORNER THE DEVICE IS AT RIGHT NOW, NOT AT ITS RESTING ONE.** The
	// response moves with the LFO, and this line is the whole of it: the same
	// `cornerHzFor` the audio path calls, handed the LFO's shape evaluated at the phase the DSP
	// published rather than at zero.
	//
	// **A SILENT PLUGIN DRAWS EXACTLY WHAT IT USED TO**, which is why this is not a change to the
	// default picture. A bridge nobody has published to answers 0, `evalLfoShape(shape, 0)` is 0 by
	// construction — `phi_lfo_shape.hpp` pins both endpoints to rest — and `modulationFor(s, 0)` is
	// what the resting version passed. Stop is the same story from the other side: `modulationFor`
	// answers Manual there whatever the tap is, so a frozen device draws a still curve.
	const float lfoNow = character::evalLfoShape(d.shape, phase());
	// **THE FREQ KNOB REACHES THE PICTURE HERE, AND A LITERAL 1.0 IS WHY IT DID NOT.** The DSP passes
	// `filterPitchRatio_ * filterFreqRatio_`; this passed 1. Note tracking is legitimately 1 on a
	// surface that is not playing a note — that half was right — but `Freq` is a KNOB, and dropping it
	// drew the curve as though the knob were permanently centred.
	const float liveHz =
	    amd::cornerHzFor(d.filterCutoffBase, d.filterModDepth, amd::modulationFor(s, lfoNow),
	                     amd::filterFreqRatioFor(s.freq), rate);


	// **THE SWEEP'S TWO EXTREMES ARE COMPUTED ONLY IF SOMETHING DRAWS THEM, AND THAT IS NOT A
	// MICRO-OPTIMISATION — IT WAS TWO THIRDS OF THIS STRIP'S ENTIRE COST.**
	//
	// MEASURED: `responseTrace()` was 9.63 µs of the strip's ~11, because it evaluated THREE curves at
	// 54 points each — the resting response plus the two ends of the corner's travel — and the
	// shipping picture draws one of them. `cornerTravel()` additionally walks a whole `previewTrace()`
	// to find the extremes, so the dead two thirds cost a fourth curve's worth of work on top.
	//
	// I REPORTED A DIFFERENT NUMBER FIRST AND IT WAS WRONG. Timing `deriveAutomod` (0.309 µs) and 54
	// magnitudes (3.431 µs) in isolation, I attributed the strip's cost to those and offered a 9%
	// saving by gating the derive. The gate is right and it stays — `view()` costs 0.398 µs against
	// `settings()`'s 0.383 — but it bought nothing measurable, because the cost was never there. The
	// only reason that is known is that the saving was measured after the change rather than asserted
	// from the profile that motivated it.
	const bool wantBand = responseStyle_ == ResponseStyle::Band;
	const CornerTravel travel = wantBand ? cornerTravel() : CornerTravel { liveHz, liveHz };


	const visage::Bounds box = responsePlot();
	const float px = std::max(box.width(), 1.0f);
	// A POINT EVERY FEW PIXELS. A response curve is smooth in log-frequency, so unlike the waveform
	// there are no corners to land on — but the same near-horizontal segment floor applies, and a
	// response is flat across most of its span by construction.
	const auto points = static_cast<size_t>(std::clamp(px / kResponsePixelsPerSample, 8.0f, 256.0f));

	ResponseTrace out;
	out.liveHz = liveHz;
	out.hz.reserve(points);
	out.gain.reserve(points);
	if (wantBand) {
		out.gainLo.reserve(points);
		out.gainHi.reserve(points);
	}
	const double lo = flt::kProfileLoHz;
	const double span = std::log2(flt::kProfileHiHz / flt::kProfileLoHz);
	for (size_t i = 0; i < points; ++i) {
		const double t = static_cast<double>(i) / static_cast<double>(points - 1);
		const auto hz = static_cast<float>(lo * std::exp2(span * t));
		out.hz.push_back(hz);
		// **THE FILTER'S OWN MAGNITUDE, FROM THE DERIVE'S OWN MIX AND RESONANCE.** Not a second model
		// of an SVF: `magnitudeAt` lives beside the kernel, takes the coefficients the kernel takes,
		// and is measured against the running filter in `test_automod_filter.cpp`.
		out.gain.push_back(amd::magnitudeAt(liveHz, d.filterResonance, d.filterMix, hz, rate));
		if (wantBand) {
			out.gainLo.push_back(amd::magnitudeAt(travel.lowHz, d.filterResonance, d.filterMix, hz, rate));
			out.gainHi.push_back(amd::magnitudeAt(travel.highHz, d.filterResonance, d.filterMix, hz, rate));
		}
	}
	return out;
}

AutomodStrip::PreviewTrace AutomodStrip::previewTrace() const {
	const amd::Settings& s = view().settings;

	// THE RATE THE STRIP DERIVES AT IS THE UN-SYNCED ONE, AND IT CANNOT MATTER TO WHAT IS DRAWN.
	// `coordsOf` documents the reason and a case pins it: `lfoHz` reaches only `springHz`,
	// `springZeta` and `combSpringHz`. The waveform, the start phase, the filter base and the mod
	// depth are all independent of it — so a surface with no transport clock draws exactly the shape
	// a synced device will play, and only the number of bounces per cycle is unknown to it.
	const amd::AutomodDerived& d = view().derived;
	const double rate = host_.sampleRate();

	// SAMPLED AT THE SEGMENT CORNERS **AND** DENSELY BETWEEN THEM, and neither half is optional.
	//
	// CORNERS ONLY would draw the right outline while hiding any defect in `evalLfoShape`'s interior
	// — which is the function the audio path reads at ARBITRARY phase, four times a tick. A picture
	// built from the breakpoints is a picture of the struct, not of the function.
	//
	// UNIFORM SAMPLES ONLY would round every corner by up to a sample's width, drawing a shape the
	// device does not play — and this waveform is nothing but corners.
	//
	// So the run boundaries are always vertices and each run is subdivided. The subdivision is not
	// decoration for the LFO trace (its runs are straight); it is what the CUTOFF trace needs, being
	// exponential in the LFO between the very same corners.
	const float px = std::max(plot().width(), 1.0f);
	const auto steps = static_cast<int>(std::max(
	    2.0f, px / (kStripPixelsPerSample * static_cast<float>(character::kLfoSegments))));

	PreviewTrace out;
	const size_t expect = character::kLfoSegments * static_cast<size_t>(steps) + 1;
	out.phase.reserve(expect);
	out.lfo.reserve(expect);
	out.cutoffHz.reserve(expect);
	const auto push = [&](float phase) {
		const float lfo = character::evalLfoShape(d.shape, phase);
		out.phase.push_back(phase);
		out.lfo.push_back(lfo);
		// **THE CORNER IS WHAT THE DEVICE WILL SIT AT, NOT WHAT THE WAVEFORM SAYS.** Depth, Manual
		// and the Stop mode go in through `modulationFor`; the field's own `filterModDepth`, the
		// bright floor and the Nyquist clamp go in through `cornerHzFor`. Both are the audio path's
		// functions, called here with `pitchRatio` 1 because a card is not playing a note.
		// THE SAME FREQ RATIO THE LIVE CORNER USES — the second of the two sites that passed a literal
		// 1.0. Fixing only the other one would have left the band and the corner travel drawn at a
		// centred knob while the live marker moved, which is a picture disagreeing with itself.
		out.cutoffHz.push_back(amd::cornerHzFor(d.filterCutoffBase, d.filterModDepth,
		                                        amd::modulationFor(s, lfo),
		                                        amd::filterFreqRatioFor(s.freq), rate));
	};

	for (size_t seg = 0; seg < character::kLfoSegments; ++seg) {
		const float from = d.shape.segStart[seg];
		const float to = (seg + 1 < character::kLfoSegments) ? d.shape.segStart[seg + 1] : 1.0f;
		// `k` STARTS AT 0 ONLY ON THE FIRST RUN. Every later run's start is the previous run's end,
		// already pushed — a duplicated vertex would be a zero-length segment for the drawing and a
		// non-increasing phase for the case that checks the axis is monotonic.
		for (int k = (seg == 0) ? 0 : 1; k <= steps; ++k) {
			push(from + (to - from) * (static_cast<float>(k) / static_cast<float>(steps)));
		}
	}
	return out;
}

AutomodStrip::Marker AutomodStrip::marker() const { return markerOn(previewTrace()); }

AutomodStrip::Marker AutomodStrip::markerOn(const PreviewTrace& trace) const {
	const visage::Bounds box = plot();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return {};
	}
	// **LOCATED ON THE TRACE THE STRIP ACTUALLY DREW, not recomputed from the shape.** The waveform
	// is drawn as straight segments between these vertices, so a marker placed by evaluating
	// `evalLfoShape` afresh would sit off the line by however much the sampling rounded a corner —
	// worst exactly at the corners, which is where a reader is looking.
	const TraceMarker::Landing where = TraceMarker::locate(trace.phase, phase());
	if (!where.valid) {
		return {};
	}
	const float inner = std::max(box.height() - 2.0f * kStripInset, 1.0f);
	const float mid = box.y() + box.height() * 0.5f;
	const auto xAt = [&](float p) { return box.x() + box.width() * p; };
	const auto lfoY = [&](float amplitude) {
		return mid - std::clamp(amplitude, -1.0f, 1.0f) * inner * 0.5f;
	};
	Marker out;
	out.at = { TraceMarker::at(where, xAt(trace.phase[where.lo]), xAt(trace.phase[where.hi])),
		       TraceMarker::at(where, lfoY(trace.lfo[where.lo]), lfoY(trace.lfo[where.hi])) };
	out.drawn = true;
	// THE LFO'S VALUE AT THE MARK, interpolated the same way its Y was — same landing, same pair of
	// vertices, so the number and the pixel cannot describe different points on the trace.
	out.lfo = TraceMarker::at(where, trace.lfo[where.lo], trace.lfo[where.hi]);
	return out;
}

void AutomodStrip::drawLfo(visage::Canvas& canvas) {
	const visage::Bounds box = plot();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}

	// THE PLATE, in the card's own vocabulary — the same fill, radius and border weight the three
	// coordinate boxes and gamma carry, so the strip reads as one more recess in this surface rather
	// than as a window onto something else.
	canvas.setColor(withAlpha(kLine, 0.12f));
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), ZoneCoordCell::kPlateRadius);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(),
	                              ZoneCoordCell::kPlateRadius, 1.0f);

	const PreviewTrace trace = previewTrace();
	if (trace.phase.size() < 2) {
		return;
	}
	const amd::Settings& s = view().settings;

	const float inner = std::max(box.height() - 2.0f * kStripInset, 1.0f);
	const float mid = box.y() + box.height() * 0.5f;
	const auto xAt = [&](float phase) { return box.x() + box.width() * phase; };
	const auto lfoY = [&](float amplitude) {
		return mid - std::clamp(amplitude, -1.0f, 1.0f) * inner * 0.5f;
	};

	// REST, so a flat waveform has something to be flat AGAINST. Drawn's curve says the same thing
	// about its unity line: without it a still device draws a line in the middle of an empty box and
	// says nothing about which line it is.
	canvas.setColor(withAlpha(kLine, 0.45f));
	canvas.segment(box.x() + kStripInset, mid, box.right() - kStripInset, mid, 1.0f, true);

	// WHERE A RETRIGGERED CYCLE BEGINS — and ONLY when a note can retrigger it. In Free the LFO is
	// never triggered at all (`Automod::setNoteContext` calls `trigger()` on a gate edge, and Free
	// simply keeps running), so a mark saying "it starts here" would be describing something that
	// does not happen. The honesty rule applied to ink rather than to a control.
	if (s.lfoMode != amd::LfoModeChoice::Free) {
		const amd::AutomodDerived& d = view().derived;
		const float x = xAt(std::clamp(d.lfoStartPhase, 0.0f, 1.0f));
		canvas.setColor(withAlpha(kMuted, 0.55f));
		canvas.segment(x, box.y() + kStripInset, x, box.bottom() - kStripInset, 1.0f, true);
	}

	// THE WAVEFORM ITSELF.
	canvas.setColor(withAlpha(kAccent, 0.9f));
	for (size_t i = 1; i < trace.phase.size(); ++i) {
		canvas.segment(xAt(trace.phase[i - 1]), lfoY(trace.lfo[i - 1]), xAt(trace.phase[i]),
		               lfoY(trace.lfo[i]), kTraceWeight, true);
	}

	// THE FOUR CORNERS THE COORDINATE AUTHORED, marked. They are what the Mod knob MOVES — the runs
	// between them are consequences — so a hand turning Mod can watch the thing it is actually
	// editing. Small and dim: this strip is not a target and a dot drawn like a handle would promise
	// a drag that phase 2 owns, which is drawn's own reasoning about its four corner marks.
	const amd::AutomodDerived& d = view().derived;
	canvas.setColor(withAlpha(kInk, 0.85f));
	for (size_t i = 0; i < character::kLfoWaypoints; ++i) {
		const float x = xAt(std::clamp(d.shape.phase[i], 0.0f, 1.0f));
		const float y = lfoY(d.shape.amplitude[i]);
		canvas.circle(x - kBreakpointRadius, y - kBreakpointRadius, kBreakpointRadius * 2.0f);
	}

	// **AND WHERE THE LFO IS RIGHT NOW**, a glowing star on the curve, and this is
	// `celestial.h`'s `starGlyph` — the same glyph the perform stage and the explore stage draw a
	// SOUND with. Borrowed rather than redrawn: a second star would be two places that must agree
	// about what a star looks like.
	//
	// **DRAWN LAST, OVER EVERYTHING INCLUDING THE CORNER DOTS.** The mark is the one element on this
	// strip that MOVES, so it is the one element a reader is tracking; ink that is being followed
	// must not sit under ink that is merely present. That is the axis labels' rule from the other
	// end — they go over the traces because they exist to be READ.
	//
	// **ASKED OF `marker()` RATHER THAN COMPUTED HERE**, so a case that checks where the mark is is
	// checking the mark that is drawn. This surface's neighbour shipped three "drawn here,
	// grabbable there" bugs before it learned that.
	if (const Marker mark = markerOn(trace); mark.drawn) {
		// THE COMB SPEAKS THROUGH THE STAR — see `combGlow`, which the case reads too.
		const CombGlow g = combGlow(mark.lfo);
		starGlyph(canvas, mark.at.x, mark.at.y, kMarkerSize, g.tint, 1.0f, g.halo);
	}
}

void AutomodStrip::drawResponse(visage::Canvas& canvas) {
	const visage::Bounds box = responsePlot();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}
	// NO PLATE OF ITS OWN — `drawLfo` already drew the box they share. A second rounded rectangle at
	// the same bounds would double the border's weight and darken the ground under one trace only.

	const ResponseTrace trace = responseTrace();
	if (trace.hz.size() < 2) {
		return;
	}

	const float inner = std::max(box.height() - 2.0f * kStripInset, 1.0f);
	const auto yAt = [&](float gain) {
		const float db = 20.0f * std::log10(std::max(gain, 1e-5f));
		const float t = std::clamp((kResponseTopDb - db) / (kResponseTopDb - kResponseBottomDb),
		                           0.0f, 1.0f);
		return box.y() + kStripInset + t * inner;
	};
	const auto xAt = [&](size_t i) {
		return box.x() + box.width() * static_cast<float>(i)
		       / static_cast<float>(trace.hz.size() - 1);
	};

	// THE SWEEP, AS A SHADED SPAN BETWEEN THE TWO ENDS OF THE CORNER'S TRAVEL.
	//
	// **FILLED AS VERTICAL COLUMNS, WHICH IS ALSO WHY IT IS SAFE TO DRAW.** visage has no polygon
	// fill, so the region between two curves is one segment per sample — and a VERTICAL segment is
	// the one case the near-horizontal artefact cannot touch (`kStripPixelsPerSample` records what a
	// shallow one does). Two outlines and a fill would have needed the artefact-prone shape twice.
	if (responseStyle_ == ResponseStyle::Band) {
		// **THE COLUMNS MUST TILE, OR THE BAND IS HATCHING.** At the density the curve wants they sit
		// about four pixels apart, and a 1.5px column leaves 2.5px of ground between each pair — which
		// photographed as vertical striping rather than as a shaded region. Widening each column to
		// its own spacing makes them abut and the region reads as one. MEASURED in a photograph, both
		// before and after; the number is the spacing rather than a taste constant, so it stays right
		// if the density changes.
		const float columnWidth =
		    std::max(1.5f, box.width() / static_cast<float>(trace.hz.size() - 1) + 0.5f);
		canvas.setColor(withAlpha(kValue, 0.42f));
		for (size_t i = 0; i < trace.hz.size(); ++i) {
			const float a = yAt(trace.gainLo[i]);
			const float b = yAt(trace.gainHi[i]);
			// A COLUMN OF ZERO HEIGHT IS NOT DRAWN: where the sweep does not move the response, the
			// two ends coincide and a zero-length rounded segment is a DOT rather than nothing.
			if (std::abs(a - b) > 0.5f) {
				canvas.segment(xAt(i), a, xAt(i), b, columnWidth, false);
			}
		}
	}

	canvas.setColor(withAlpha(kValue, 0.9f));
	for (size_t i = 1; i < trace.hz.size(); ++i) {
		canvas.segment(xAt(i - 1), yAt(trace.gain[i - 1]), xAt(i), yAt(trace.gain[i]), kTraceWeight,
		               true);
	}
}

void AutomodStrip::drawAxes(visage::Canvas& canvas) {
	const visage::Bounds box = plot();
	if (box.width() <= 1.0f || picture_ == Picture::LfoOnly) {
		return;
	}

	// **THE LABELS ARE THE DESIGN, NOT DECORATION ON IT.** Two curves whose x positions mean
	// different things will be read as related unless something stops it — a reader seeing the LFO's
	// peak above a dip in the response will infer a connection that does not exist. So each axis is
	// named at its OWN EDGE in its OWN TRACE'S COLOUR: frequency along the top in amber, time along
	// the bottom in blue. Neither scale is anonymous and neither shares an edge with the other.
	const visage::Font& face = font(kKnobLabelSize);
	const float lo = static_cast<float>(flt::kProfileLoHz);
	const float span = static_cast<float>(std::log2(flt::kProfileHiHz / flt::kProfileLoHz));

	// FREQUENCY, ALONG THE TOP, IN THE RESPONSE'S COLOUR. Decade marks only — 100 / 1k / 10k is
	// enough to fix the scale, and a full ruler would compete with two curves for the same pixels.
	//
	// **ON PLATES, BECAUSE THE CURVES GO WHERE THE LABELS ARE.** The first shot of this strip put
	// `10k` under the waveform's peak and ran the response straight through `100` and `1k` — both
	// unreadable. There is no corner of a box holding two full-width curves that a placement rule can
	// keep clear, which is the same conclusion the first version's `21.6 kHz` label reached; this
	// extends that convention rather than inventing a second one.
	const auto plated = [&](const std::string& text, float x, float y, uint32_t colour) {
		const float w = trackedWidth(face, text, kKnobLabelTracking);
		canvas.setColor(withAlpha(kSurface, 0.9f));
		canvas.roundedRectangle(x - 3.0f, y, w + 6.0f, Knob::kLabelHeight, 3.0f);
		canvas.setColor(colour);
		drawTracked(canvas, text, face, kKnobLabelTracking, x, y, Knob::kLabelHeight);
	};

	for (const auto& [hz, text] : { std::pair { 100.0f, "100" }, std::pair { 1000.0f, "1k" },
	                                std::pair { 10000.0f, "10k" } }) {
		const float t = std::log2(hz / lo) / span;
		const float x = box.x() + box.width() * t;
		canvas.setColor(withAlpha(kValue, 0.75f));
		canvas.segment(x, box.y() + 1.0f, x, box.y() + 4.0f, 1.0f, true);
		const float w = trackedWidth(face, text, kKnobLabelTracking);
		plated(text, x - w * 0.5f, box.y() + 4.0f, withAlpha(kValue, 0.85f));
	}

	// TIME, ALONG THE BOTTOM, IN THE WAVEFORM'S COLOUR. Quarter-cycle ticks and one word, because the
	// unit a player cares about is "one cycle" rather than any particular number of them.
	canvas.setColor(withAlpha(kAccent, 0.75f));
	for (int q = 0; q <= 4; ++q) {
		const float x = box.x() + box.width() * (static_cast<float>(q) * 0.25f);
		canvas.segment(x, box.bottom() - 4.0f, x, box.bottom() - 1.0f, 1.0f, true);
	}
	//
	// **BOTTOM-LEFT IS A PREFERENCE, NOT A CLEARANCE — AND THE FIRST VERSION OF THIS COMMENT CLAIMED
	// OTHERWISE.** It argued that a cycle starts and ends at REST (`phi_lfo_shape.hpp` pins both
	// endpoints to zero), so the two bottom corners were the one place the blue trace could not be.
	// **The photograph disproved it immediately**: the waveform leaves rest and DESCENDS, straight
	// through the label.
	//
	// There is no corner of a box holding a full-width waveform that the waveform cannot reach. The
	// PLATE is the mechanism here exactly as it is for the frequency labels above; the corner is only
	// a tiebreak between two placements that interrupt the trace about equally.
	plated("1 CYCLE", box.x() + kStripInset + 2.0f, box.bottom() - 4.0f - Knob::kLabelHeight,
	       withAlpha(kAccent, 0.85f));
}

void AutomodStrip::drawEnvelope(visage::Canvas& canvas) {
	const visage::Bounds box = plot();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}

	// OPACITY IS THE STEERING ITSELF — see `envTrace`.
	// NO EARLY-OUT ON ALPHA ANY MORE. It skipped the draw below 0.004, which was right when the trace
	// could fade to nothing and is wrong now that it has a floor — the floor's whole job is to be
	// drawn while idle, and a guard that skipped it would reinstate exactly the invisibility it was
	// added to remove.
	const EnvTrace ink = envTrace();

	// THE SAME GEOMETRY THE WAVEFORM USES, taken from the same two numbers rather than restated — a
	// trace on a different inset would read as a different plot sharing a box.
	const float inner = std::max(box.height() - 2.0f * kStripInset, 1.0f);
	const float mid = box.y() + box.height() * 0.5f;

	// THE LEVEL IS DRAWN UPWARD FROM REST, because it is a magnitude and rest is where it sits when
	// nothing is playing. The waveform is bipolar about `mid` and this is not, which is the one place
	// the two pictures deliberately disagree about what the vertical axis means — and it is why the
	// trace is a LINE rather than a second curve: a line at a height cannot be mistaken for a shape.
	const float level = std::clamp(envelope(), 0.0f, 1.0f);
	const float y = mid - level * inner * 0.5f;

	// A NAME OF ITS OWN RATHER THAN `kTraceWeight`, which is already a file-scope constant at 2.0 for
	// the curves. Shadowing it here would compile and would leave two different numbers under one name
	// in one file.
	constexpr float kEnvelopeWeight = 1.0f;
	canvas.setColor(withAlpha(ink.ink, ink.alpha));
	canvas.rectangle(box.x(), y - kEnvelopeWeight * 0.5f, box.width(), kEnvelopeWeight);
}

void AutomodStrip::draw(visage::Canvas& canvas) {
	// **ORDER IS THE DESIGN AGAIN**: the response goes under the waveform because the waveform is
	// what the coordinates AUTHOR, and the axis marks go over both because ink that exists to be
	// READ must never sit under ink that exists to be FOLLOWED — the lesson the first version's
	// buried `21.6 kHz` label paid for.
	drawLfo(canvas);
	drawResponse(canvas);
	// THE ENVELOPE OVER BOTH CURVES AND UNDER THE AXES, by the ordering rule stated above: it is ink
	// to be READ against the shapes, so it must not sit beneath them — and the axis marks outrank it
	// for the same reason they outrank the waveform.
	drawEnvelope(canvas);
	drawAxes(canvas);
}

} // namespace feathers::ui
