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

// THE EQ, MINIMUM PHASE (step 2). Eight bands and a tilt, each a cascade of
// SVF / one-pole sections designed by `eq_design.h`, with an output trim.
//
// CONTROL ON A FIXED 32-SAMPLE GRID, Drawn's pattern (`drawn.h`): the grid's phase is carried across
// calls, so however the host splits its blocks the coefficients change at the same samples. Each grid
// step smooths the band and global targets (one-pole, `kSmoothSeconds`, in log2 Hz / dB / log2 Q), then
// re-resolves and REDESIGNS ONLY THE BANDS WHOSE RESOLVED SETTINGS MOVED — so a still EQ designs nothing.
// Modulation arrives through `setFields` once per control tick and is smoothed by the same grid.

#include "fx/eq/eq_linear.h"
#include "fx/eq/eq_types.h"
#include "fx/dyn/meter_ramp.h"
#include "dsp/analyser.h"
#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace feathers::fx::eq {

namespace {

constexpr int kGridSamples = 32;

/// THE SMOOTHER'S TIME CONSTANT (doc §2.4: "~5 ms"). ASSUMPTION A5 (provisional): long enough to turn a
/// control tick's step into a glide, short enough that a hand-drawn sweep arrives with the hand.
constexpr double kSmoothSeconds = 0.005;

/// A smoothed value that LANDS: within this of its target it snaps, so a gain smoothing toward 0 dB
/// reaches exactly 0 dB and the band bypasses (Drawn's "1e-6 settled Wake 0 exactly").
constexpr double kSnap = 1e-6;

/// THE REST (MultimodeFilter's and CLIP's rule, `multimode_filter.h:105-152`, `clip.cpp:60`): once the
/// input has been EXACTLY zero for `kRestSilentSeconds` and every output sample in that time was under
/// `kRestLevel`, the state is cleared and the output is exact zero until the input moves again. A
/// decaying IIR otherwise walks into the denormals and parks, and a +24 dB, Q 18 bell at 20 Hz takes
/// ~24 s to fall 180 dB (derived, doc §2.7). ASSUMPTION A6 (provisional): CLIP's constants — -180 dBFS
/// and 50 ms. Nothing at -180 dBFS is audible, and the input condition is what protects a state that
/// holds more than its output shows (a cut answering DC with silence).
constexpr float kRestLevel = 1.0e-9f;
constexpr double kRestSilentSeconds = 0.05;

/// THE SAFETY RAIL, +80 dBFS. Eight bands at Amount -200% x -24 dB are eight +48 dB bells, and pushed to
/// the floor together by Offset they stack to ~+288 dB — the settings' real arithmetic, not an
/// instability (test_fx_devices' hot sweep found exactly that: its min-everything row). Nothing the host
/// can use lives above this, and CLIP's `kNormalisedClamp` is the same number for the same reason.
/// ASSUMPTION A7 (provisional): a clamp on the output only; no normal setting comes within 60 dB of it.
constexpr float kOutputClamp = 1.0e4f;

[[nodiscard]] inline float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

struct Smoothed {
	double value = 0.0;
	double target = 0.0;

	void step(double alpha) noexcept {
		value += alpha * (target - value);
		if (std::fabs(target - value) < kSnap) {
			value = target;
		}
	}
	void snap() noexcept { value = target; }
};

[[nodiscard]] bool sameDesign(const EffectiveBand& a, const EffectiveBand& b) {
	return a.type == b.type && a.on == b.on && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q
	       && a.slopeIndex == b.slopeIndex;
}

/// ONE REALISATION OF A BAND: its sections' coefficients and both channels' state.
struct Realised {
	bool bypass = true;
	int count = 0;
	std::array<bool, kMaxSectionsPerBand> firstOrder {};
	std::array<SvfCoefs, kMaxSectionsPerBand> svf {};
	std::array<OnePoleCoefs, kMaxSectionsPerBand> one {};
	std::array<std::array<SvfState, kMaxSectionsPerBand>, 2> svfState {};
	std::array<std::array<OnePoleState, kMaxSectionsPerBand>, 2> oneState {};

	void clear() noexcept {
		for (auto& ch : svfState) {
			for (SvfState& s : ch) {
				s.clear();
			}
		}
		for (auto& ch : oneState) {
			for (OnePoleState& s : ch) {
				s.clear();
			}
		}
	}

	[[nodiscard]] bool sameTopology(const BandDesign& d) const noexcept {
		if (d.bypass != bypass || d.count != count) {
			return false;
		}
		for (int i = 0; i < d.count; ++i) {
			if (d.sections[static_cast<size_t>(i)].firstOrder != firstOrder[static_cast<size_t>(i)]) {
				return false;
			}
		}
		return true;
	}

	void setCoefs(const BandDesign& d) noexcept {
		bypass = d.bypass;
		count = d.count;
		for (int i = 0; i < d.count; ++i) {
			const Section& s = d.sections[static_cast<size_t>(i)];
			firstOrder[static_cast<size_t>(i)] = s.firstOrder;
			if (s.firstOrder) {
				one[static_cast<size_t>(i)] = onePoleFromBiquad(s.z);
			}
			else {
				svf[static_cast<size_t>(i)] = svfFromBiquad(s.z);
			}
		}
	}

	[[nodiscard]] float run(int ch, float x) noexcept {
		if (bypass) {
			return x;
		}
		auto& ss = svfState[static_cast<size_t>(ch)];
		auto& os = oneState[static_cast<size_t>(ch)];
		for (int i = 0; i < count; ++i) {
			const auto k = static_cast<size_t>(i);
			x = firstOrder[k] ? os[k].step(one[k], x) : ss[k].step(svf[k], x);
		}
		return x;
	}
};

/// THE CROSSFADE THAT REPLACED A CLICK (assumption A9, resolved in step 3). When a band changes what it
/// IS — type, on/off, slope, or the shape of its cascade — the new filter starts from zero state, and a
/// hard switch from a charged filter to an empty one is a step in the output. Instead the old realisation
/// keeps running beside the new one for `kFadeSeconds` and the output crossfades between them. The cost
/// is one band's work twice, for 5 ms, only at a change. A change of COEFFICIENTS alone keeps the state
/// and needs no fade: that is what the SVF topology is for.
constexpr double kFadeSeconds = 0.005;

/// A CHANNEL CHANGE IS A FADE OUT AND A FADE IN (step 6, assumption A19). The band's two realisations
/// cannot crossfade across a change of channel: the old one's output is a left or right signal and the new
/// one's a mid or side, and there is no one point in the signal path where both exist. So the band first
/// fades to IDENTITY in the channel it is in (`leaveLeft`, `kFadeSeconds`), then moves, starts from zero
/// state, and fades in from identity (the ordinary `fading` road, with a bypassed realisation to fade
/// from). 10 ms in all, only at a change; a band that is bypassed moves at once.
struct BandRun {
	Smoothed freqLog2, gainDb, qLog2;
	BandType type = BandType::Bell;
	bool on = false;
	int slope = kDefaultSlope;
	Channel channel = Channel::Stereo;     ///< where it runs now
	Channel wantChannel = Channel::Stereo; ///< where the extra-state block says it runs
	BandDesign design {};                  ///< the design `live` realises (linear phase builds from it)

	EffectiveBand designedFor {};
	bool designed = false;
	Realised live {};
	Realised fading {};
	int fadeLeft = 0;
	int leaveLeft = 0;
	int fadeLength = 240;

	void clear() noexcept {
		live.clear();
		fading.clear();
		fadeLeft = 0;
		// A move waiting on its fade out is made now: the state it would have faded is gone anyway.
		if (leaveLeft > 0) {
			channel = wantChannel;
			leaveLeft = 0;
		}
	}

	[[nodiscard]] bool bypass() const noexcept { return live.bypass && fadeLeft == 0 && leaveLeft == 0; }

	/// Take a new design. `snap` is the first design after a start or reset: there is nothing to fade from.
	void adopt(const BandDesign& d, bool changedKind, bool snap) noexcept {
		const bool restart = !live.sameTopology(d) || changedKind;
		if (restart && !snap) {
			fading = live;
			fadeLeft = fadeLength;
		}
		live.setCoefs(d);
		if (restart) {
			live.clear();
		}
	}

	/// Once per grid step: begin a move to `wantChannel`, or make it at once where nothing can click.
	void aimChannel(bool snap) noexcept {
		if (wantChannel == channel || leaveLeft > 0) {
			return;
		}
		if (snap || bypass()) {
			channel = wantChannel;
			clear();
			return;
		}
		leaveLeft = fadeLength;
	}

	[[nodiscard]] float run(int ch, float x) noexcept {
		float y = live.run(ch, x);
		if (fadeLeft > 0) {
			const float t = static_cast<float>(fadeLeft) / static_cast<float>(fadeLength); // 1 -> 0
			y += t * (fading.run(ch, x) - y);
		}
		if (leaveLeft > 0) {
			const float t = static_cast<float>(leaveLeft) / static_cast<float>(fadeLength); // 1 -> 0
			y = x + t * (y - x);
		}
		return y;
	}

	/// Once per sample, after both channels.
	void tick() noexcept {
		if (fadeLeft > 0) {
			--fadeLeft;
		}
		if (leaveLeft > 0 && --leaveLeft == 0) {
			// FADED OUT: move, and fade in from identity.
			channel = wantChannel;
			live.clear();
			fading.bypass = true;
			fading.count = 0;
			fadeLeft = fadeLength;
		}
	}
};

/// THE ANALYSER, AT THIS DEVICE'S CONTROL GRID — lifted to `dsp/analyser.h` (rack stage 2, §7a).
using Analyser = dsp::Analyser<kGridSamples>;

/// Write the analyser's last frame into `values` in `Reading` order (the counter both ends).
void writeAnalyserTo(const Analyser& a, float* values) noexcept {
	values[static_cast<size_t>(Reading::FrameA)] = a.frame();
	values[static_cast<size_t>(Reading::View)] = static_cast<float>(a.frameView());
	for (size_t k = 0; k < kAnalyserBins; ++k) {
		values[static_cast<size_t>(Reading::PreFirst) + k] = a.pre()[k];
		values[static_cast<size_t>(Reading::PostFirst) + k] = a.post()[k];
	}
	values[static_cast<size_t>(Reading::FrameB)] = a.frame();
}

struct OpsEq {
	Settings state {};
	double sampleRate = 48000.0;
	double alpha = 1.0;

	std::array<BandRun, kBandCount> bands {};
	Smoothed offset, amount, tilt, pivotLog2;

	double designedTilt = 0.0, designedPivot = 0.0;
	bool tiltBypass = true;
	float tiltGain = 1.0f;
	std::array<OnePoleCoefs, kTiltSections> tiltCoefs {};
	std::array<std::array<OnePoleState, kTiltSections>, 2> tiltState {};

	dyn::LinearRamp output, bypass;
	dyn::HeldPeak outPeak;
	device::ReadingsSink sink {};
	Analyser analyser;
	std::array<float, kExtraState> extra {};
	std::array<float, kReadingCount> readings {};

	bool primed = false;
	int gridPhase = 0;
	int restSamples = 2400;
	int quietRun = 0;
	bool resting = false;
	bool offCleared = false;
	bool midSideActive = false;
	/// KEY TRACK (backlog MQ): the last note heard, as octaves from C3 — latched always, so switching Key Track on picks it
	/// up; 0 (C3, no shift) until a note arrives, which is the bus with nothing played.
	double noteOctaves = 0.0;
	TiltDesign tiltDesign {};

	// LINEAR PHASE (step 7): the FIR, whether it is the path running, whether a new kernel is wanted, the
	// channels the last request was made for, and the DRY delayed by the same latency (for the enable fade,
	// the off path and the analyser's PRE tap), all allocated in `prepare`.
	LinearPhase linear;
	bool linearOn = false;
	bool linearDirty = false;
	std::array<Channel, kBandCount> builtChannel {};
	uint32_t latency = 0;
	std::vector<float> dryL, dryR;
	size_t dryMask = 0, dryWrite = 0;
	int linearSpan = 0; ///< N + 2B: how long the FIR can still sound after its input stops

	[[nodiscard]] LinearPhase::Target target() const noexcept {
		LinearPhase::Target t;
		for (size_t b = 0; b < bands.size(); ++b) {
			t.bands[b] = bands[b].design;
			t.channel[b] = bands[b].wantChannel;
		}
		t.tilt = tiltDesign;
		return t;
	}

	/// THE DRY, as late as the FIR: written and read every sample while linear.
	void dryDelay(float xL, float xR, float& dL, float& dR) noexcept {
		const size_t read = (dryWrite - latency) & dryMask;
		dryL[dryWrite] = xL;
		dryR[dryWrite] = xR;
		dL = dryL[read];
		dR = dryR[read];
		dryWrite = (dryWrite + 1) & dryMask;
	}

	void clearLinear() noexcept {
		linear.clearAudio();
		std::fill(dryL.begin(), dryL.end(), 0.0f);
		std::fill(dryR.begin(), dryR.end(), 0.0f);
		dryWrite = 0;
	}

	/// INTO OR OUT OF LINEAR PHASE: a fresh start either way (the host restarts on the latency change). Into
	/// it, the kernel is built whole, now — there is none to crossfade from.
	void switchPhase(bool toLinear) noexcept {
		linearOn = toLinear;
		latency = toLinear ? linearLatency(sampleRate) : 0;
		clearAll();
		if (toLinear) {
			linear.buildNow(target());
			linearDirty = false;
		}
		quietRun = 0;
	}

	void clearTilt() noexcept {
		for (auto& ch : tiltState) {
			for (OnePoleState& s : ch) {
				s.clear();
			}
		}
	}

	void clearAll() noexcept {
		for (BandRun& b : bands) {
			b.clear();
		}
		clearTilt();
		clearLinear();
	}

	/// The targets from the params. Stepped fields (type, on, slope) take effect at the next grid step.
	void aim() noexcept {
		for (int i = 0; i < kBandCount; ++i) {
			const Settings::BandParams& p = state.bands[static_cast<size_t>(i)];
			BandRun& b = bands[static_cast<size_t>(i)];
			b.freqLog2.target = p.freqLog2;
			b.gainDb.target = p.gainDb;
			b.qLog2.target = p.qLog2;
			b.type = p.type;
			b.on = p.on;
			b.slope = p.slopeIndex;
			b.wantChannel = channelOf(extra[static_cast<size_t>(channelField(i))]);
		}
		// KEY TRACK RIDES ON OFFSET: both move every band by octaves (resolve), and the smoother glides a new note in. Off
		// is the stored Offset itself, not a sum with zero: today's EQ to the bit.
		offset.target = state.keyTrack ? static_cast<double>(state.offsetOctaves) + noteOctaves : state.offsetOctaves;
		amount.target = state.amount;
		tilt.target = state.tiltDbPerOctave;
		pivotLog2.target = state.tiltPivotLog2;
		output.set(std::exp2(state.outputDb / 6.020599913279624f));
		bypass.set(state.enabled ? 1.0f : 0.0f);
	}

	/// ONE GRID STEP: smooth, resolve, redesign what moved.
	void control(bool snapAll) noexcept {
		bool moved = false;
		for (BandRun& b : bands) {
			for (Smoothed* s : { &b.freqLog2, &b.gainDb, &b.qLog2 }) {
				snapAll ? s->snap() : s->step(alpha);
			}
		}
		for (Smoothed* s : { &offset, &amount, &tilt, &pivotLog2 }) {
			snapAll ? s->snap() : s->step(alpha);
		}
		const Globals g { offset.value, amount.value, tilt.value, std::exp2(pivotLog2.value) };
		for (BandRun& b : bands) {
			b.aimChannel(snapAll || !b.designed);
			const Band band { b.type, b.on, std::exp2(b.freqLog2.value), b.gainDb.value, std::exp2(b.qLog2.value),
				              b.slope };
			const EffectiveBand e = resolve(band, g, sampleRate);
			if (!b.designed || !sameDesign(e, b.designedFor)) {
				const EffectiveBand& was = b.designedFor;
				const bool cut = e.type == BandType::LowCut || e.type == BandType::HighCut;
				const bool changedKind = b.designed
				                         && (e.type != was.type || e.on != was.on || (cut && e.slopeIndex != was.slopeIndex));
				b.design = design(e, sampleRate);
				b.adopt(b.design, changedKind, snapAll || !b.designed);
				moved = true;
				b.designedFor = e;
				b.designed = true;
			}
		}
		const double effectiveTilt = g.tiltDbPerOctave * g.amount;
		if (effectiveTilt != designedTilt || g.tiltPivotHz != designedPivot || snapAll) {
			const TiltDesign t = designTilt(effectiveTilt, g.tiltPivotHz, sampleRate);
			if (t.bypass != tiltBypass) {
				clearTilt();
			}
			tiltBypass = t.bypass;
			tiltGain = static_cast<float>(t.gain);
			tiltDesign = t;
			moved = true;
			for (int i = 0; i < kTiltSections; ++i) {
				tiltCoefs[static_cast<size_t>(i)] = onePoleFromBiquad(t.sections[static_cast<size_t>(i)]);
			}
			designedTilt = effectiveTilt;
			designedPivot = g.tiltPivotHz;
		}
		// LINEAR PHASE builds from the same designs, on each band's WANTED channel (a kernel crossfade replaces the
		// minimum-phase fade out and in). Anything that moved asks for a new kernel.
		for (size_t b = 0; b < bands.size(); ++b) {
			if (bands[b].wantChannel != builtChannel[b]) {
				builtChannel[b] = bands[b].wantChannel;
				moved = true;
			}
		}
		linearDirty = linearDirty || moved;
	}

	/// THE SIGNAL PATH (doc §7). Filtering is linear and time-invariant, so the bands may be reordered: the
	/// Stereo bands run on left and right; then, ONLY IF a Mid or Side band is acting, the pair is converted
	/// to mid and side, each runs its own bands, and it is converted back. A patch with no M/S band never
	/// takes the round trip, which in floats is not exact. The tilt is the same on both channels, so it
	/// commutes with all of it and runs last.
	void wet(float& l, float& r) noexcept {
		bool midSide = false;
		for (BandRun& b : bands) {
			if (b.bypass()) {
				continue;
			}
			if (b.channel == Channel::Stereo) {
				l = b.run(0, l);
				r = b.run(1, r);
			}
			else {
				midSide = true;
			}
		}
		midSideActive = false;
		if (midSide) {
			midSideActive = true;
			float m = 0.5f * (l + r);
			float s = 0.5f * (l - r);
			for (BandRun& b : bands) {
				if (b.bypass() || b.channel == Channel::Stereo) {
					continue;
				}
				if (b.channel == Channel::Mid) {
					m = b.run(0, m);
				}
				else {
					s = b.run(0, s);
				}
			}
			l = m + s;
			r = m - s;
		}
		if (!tiltBypass) {
			for (int ch = 0; ch < 2; ++ch) {
				float& x = ch == 0 ? l : r;
				auto& ts = tiltState[static_cast<size_t>(ch)];
				for (int i = 0; i < kTiltSections; ++i) {
					x = ts[static_cast<size_t>(i)].step(tiltCoefs[static_cast<size_t>(i)], x);
				}
				x *= tiltGain;
			}
		}
	}

	void process(float* left, float* right, int frames) noexcept {
		aim();
		if (!primed) {
			control(true);
			primed = true;
			gridPhase = 0;
		}
		// THE PHASE THE HOST WAS TOLD (`latencySamples` reads the same stored field): switch at once.
		if (state.linearPhase != linearOn) {
			switchPhase(state.linearPhase);
		}
		for (int i = 0; i < frames; ++i) {
			if (gridPhase == 0) {
				control(false);
				analyser.step();
				if (linearOn) {
					if (linearDirty && linear.request(target())) {
						linearDirty = false;
					}
					linear.step();
				}
			}
			gridPhase = (gridPhase + 1) % kGridSamples;

			const float xL = std::isfinite(left[i]) ? left[i] : 0.0f;
			const float xR = std::isfinite(right[i]) ? right[i] : 0.0f;
			const bool silentIn = xL == 0.0f && xR == 0.0f;
			const float gOut = output.next();
			const float b = bypass.next();

			if (resting && silentIn) {
				left[i] = 0.0f;
				right[i] = 0.0f;
				outPeak.push(0.0f);
				analyser.push(0.0f, 0.0f, 0.0f, 0.0f);
				continue;
			}
			resting = false;

			// THE DRY THIS SAMPLE MEETS: the input, or in linear phase the input as late as the FIR.
			float dL = xL, dR = xR;
			float yL = xL, yR = xR;
			if (linearOn) {
				dryDelay(xL, xR, dL, dR);
				// ALWAYS MID AND SIDE (eq_linear.h), and run even while off so switching on finds it charged.
				float m = 0.0f, s = 0.0f;
				linear.process(0.5f * (xL + xR), 0.5f * (xL - xR), m, s);
				yL = m + s;
				yR = m - s;
			}

			if (b == 0.0f && bypass.settled()) {
				// OFF AND SETTLED: exactly the dry, and (minimum phase) a fresh start when it comes back on
				// (cleared once, not every sample).
				if (!offCleared && !linearOn) {
					clearAll();
					offCleared = true;
				}
				left[i] = dL;
				right[i] = dR;
				outPeak.push(std::max(std::fabs(dL), std::fabs(dR)));
				analyser.push(dL, dR, dL, dR);
				quietRun = 0;
				continue;
			}

			offCleared = false;
			if (!linearOn) {
				wet(yL, yR);
				for (BandRun& band : bands) {
					band.tick();
				}
			}
			yL *= gOut;
			yR *= gOut;
			if (b != 1.0f) {
				yL = (1.0f - b) * dL + b * yL;
				yR = (1.0f - b) * dR + b * yR;
			}
			if (!std::isfinite(yL) || !std::isfinite(yR)) {
				clearAll();
				yL = finiteOr0(yL);
				yR = finiteOr0(yR);
			}
			yL = std::clamp(yL, -kOutputClamp, kOutputClamp);
			yR = std::clamp(yR, -kOutputClamp, kOutputClamp);
			left[i] = yL;
			right[i] = yR;
			outPeak.push(std::max(std::fabs(yL), std::fabs(yR)));
			analyser.push(dL, dR, yL, yR);

			// THE REST: in linear phase only once the FIR's whole span has passed with nothing in.
			if (silentIn && std::fabs(yL) < kRestLevel && std::fabs(yR) < kRestLevel) {
				if (++quietRun >= restSamples + (linearOn ? linearSpan : 0)) {
					clearAll();
					resting = true;
				}
			}
			else {
				quietRun = 0;
			}
		}
		publish();
	}

	/// THE READINGS: the output meter (R14) — the post-trim peak, held for the reader the way COMP's is — and
	/// the analyser's latest frame, counter first and last (`Reading`).
	void publish() noexcept {
		if (sink.publish != nullptr) {
			readings[static_cast<size_t>(Reading::OutPeak)] = outPeak.reading();
			writeAnalyserTo(analyser, readings.data());
			readings[static_cast<size_t>(Reading::KeyOctaves)] = state.keyTrack ? static_cast<float>(noteOctaves) : 0.0f;
			sink.publish(sink.context, readings.data(), readings.size());
		}
	}

	/// Allocates (the analyser's rings and transform), which is why it is `prepare`, off the audio thread.
	void prepare(double rate) {
		sampleRate = std::max(rate, 1.0);
		alpha = 1.0 - std::exp(-static_cast<double>(kGridSamples) / (kSmoothSeconds * sampleRate));
		restSamples = std::max(1, static_cast<int>(std::lround(kRestSilentSeconds * sampleRate)));
		output.prepare(sampleRate);
		bypass.prepare(sampleRate);
		outPeak.prepare(sampleRate);
		analyser.prepare(sampleRate);
		linear.prepare(sampleRate);
		linearSpan = static_cast<int>(linear.firLength() + 2 * linear.blockLength());
		size_t ring = 1;
		while (ring < static_cast<size_t>(linearLatency(sampleRate)) + 1) {
			ring <<= 1;
		}
		dryL.assign(ring, 0.0f);
		dryR.assign(ring, 0.0f);
		dryMask = ring - 1;
		for (BandRun& b : bands) {
			b.fadeLength = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
		}
		reset();
	}

	/// AS IF JUST SWITCHED ON: the state is forgotten, and so is the fact that anything was running — the
	/// next block snaps every smoother to its target instead of gliding in from the last settings.
	void reset() noexcept {
		clearAll();
		output.reset();
		bypass.reset();
		outPeak.reset();
		analyser.reset();
		for (BandRun& b : bands) {
			b.designed = false;
		}
		primed = false;
		gridPhase = 0;
		quietRun = 0;
		resting = false;
		midSideActive = false;
		// A RESTART (the host's, after a Phase toggle among others): the next block re-enters whichever phase
		// is stored and, if linear, builds its kernel whole from the settings it then has.
		linearOn = false;
		latency = 0;
	}
};

OpsEq& asEq(void* storage) {
	return *static_cast<OpsEq*>(storage);
}
const OpsEq& asEq(const void* storage) {
	return *static_cast<const OpsEq*>(storage);
}

} // namespace

EffectiveBand effectiveForTest(const void* storage, int band) {
	return asEq(storage).bands[static_cast<size_t>(band)].designedFor;
}

double tiltForTest(const void* storage) {
	return asEq(storage).designedTilt;
}

bool bandBypassedForTest(const void* storage, int band) {
	return asEq(storage).bands[static_cast<size_t>(band)].bypass();
}

int analyserTransformsForTest(const void* storage) {
	return asEq(storage).analyser.fftsForTest();
}

Channel bandChannelForTest(const void* storage, int band) {
	return asEq(storage).bands[static_cast<size_t>(band)].channel;
}

bool midSideActiveForTest(const void* storage) {
	return asEq(storage).midSideActive;
}

int linearBuildsForTest(const void* storage) {
	return asEq(storage).linear.buildsForTest();
}

int linearSwapsForTest(const void* storage) {
	return asEq(storage).linear.swapsForTest();
}

const device::AudioOps kAudioOps {
	.stateBytes = sizeof(OpsEq),
	.stateAlign = alignof(OpsEq),
	.construct = [](void* storage) { new (storage) OpsEq(); },
	.destruct = [](void* storage) { asEq(storage).~OpsEq(); },
	.prepare = [](void* storage, double sampleRate, int) { asEq(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asEq(storage).reset(); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    Row row {};
		    const size_t take = std::min(fieldsPresent, row.size());
		    for (size_t i = 0; i < take; ++i) {
			    row[i] = fields[i];
		    }
		    OpsEq& ops = asEq(storage);
		    ops.state = stateOf(row, take);
		    // THE EXTRA-STATE BLOCK rides behind the params (`Traits::extraState`); unwritten, it reads as zeros.
		    ops.extra.fill(0.0f);
		    for (size_t i = kParamCount; i < std::min(fieldsPresent, kParamCount + kExtraState); ++i) {
			    ops.extra[i - kParamCount] = fields[i];
		    }
		    ops.analyser.setView(static_cast<dsp::AnalyserView>(channelOf(ops.extra[static_cast<size_t>(Extra::AnalyserChannel)])));
		    ops.analyser.setTaps(ops.extra[static_cast<size_t>(Extra::Pre)] < 0.5f,
		                         ops.extra[static_cast<size_t>(Extra::Post)] < 0.5f);
	    },
	// THE NOTE ROAD: Key Track's f0 (backlog MQ). On the bus it is the last note the host hands every device.
	.setNoteContext =
	    [](void* storage, const device::NoteContext& note) {
		    asEq(storage).noteOctaves = keyTrackOctaves(static_cast<double>(note.pitchSemitones));
	    },
	.processStereo = [](void* storage, float* left, float* right,
	                    int frames) { asEq(storage).process(left, right, frames); },
	.processMono = nullptr,
	.setReadingsSink = [](void* storage, device::ReadingsSink sink) { asEq(storage).sink = sink; },
	.setOuterSources = nullptr,
	.beginPerVoiceBlock = nullptr,
	.processVoice = nullptr,
	.windowedControls = {},
	.setFieldWindows = nullptr,
	.latencySamples =
	    [](const void*, const float* fields, size_t fieldsPresent, double sampleRate) -> uint32_t {
		    // A PURE FUNCTION OF THE STORED PHASE AND THE RATE (JM's L1), decoded as `stateOf` decodes it.
		    const size_t i = paramIndex(Param::Phase);
		    const float v = i < fieldsPresent ? fields[i] : static_cast<float>(kParamDescs[i].def);
		    const bool linear = std::isfinite(v) && std::clamp(static_cast<int>(std::lround(v)), 0, 1) == 1;
		    return linear ? linearLatency(sampleRate) : 0;
	    },
};

} // namespace feathers::fx::eq
