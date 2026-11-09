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

#include "fx/comp/comp_dsp.h"

#include <algorithm>
#include <array>
#include <cmath>

// COMP'S RENDER PATH (`comp_dsp.h` holds the type; `comp_ops.cpp` the holder and the op table).

namespace feathers::fx::comp {

void CompDsp::process(float* left, float* right, int frames) {
	aimRamps();
	const bool limit = state.mode == static_cast<int>(Mode::Limit);
	const bool rms = state.detect == static_cast<int>(Detect::Rms);
	const bool hp = hpHz > 0.0f;
	// SIDE IS COMPRESS'S ONLY: Limit holds a ceiling on its own audio (`Param::Detector`), so it never reads the sidechain.
	const bool side = !limit && state.detector == static_cast<int>(Detector::Side);
	const bool ms = state.stereoMode == static_cast<int>(StereoMode::MidSide);
	if (late.empty()) {
		return; // NOT PREPARED: a host that runs a device it never prepared hears it unchanged.
	}
	// **THE COMPANDER HAS ITS OWN ROAD** (backlog QG): two passes around a lane, which a per-sample loop cannot hold.
	if (state.mode == static_cast<int>(Mode::Compander)) {
		processCompander(left, right, frames);
		return;
	}
	// THE COMPANDER'S DRY PAD IS NOT IN USE HERE: forgotten, so a return to Compander starts it from silence, not replaying.
	dryPad.reset();
	for (int i = 0; i < frames; ++i) {
		// **A NON-FINITE INPUT SAMPLE IS TREATED AS 0**, before anything sees it. One NaN reaching the
		// smoother latched it for good (`target < gainDb` is false against NaN, so the attack branch
		// never runs again), and an inf made NaN through the curve and poisoned the SC HPF and the mean
		// squares. Treated as silence, it costs one sample of release and the device stays on the
		// trajectory it would have had — test "one NaN or inf sample does not latch". (The disabled
		// pass-through below does not look: a device that is off changes no sample, NaN included.)
		const float xL = std::isfinite(left[i]) ? left[i] : 0.0f;
		const float xR = std::isfinite(right[i]) ? right[i] : 0.0f;
		inPeak.push(std::max(std::fabs(xL), std::fabs(xR)));
		// **WHAT ENTERS THE DELAY THIS SAMPLE** (backlog LE): Compress's finished output, or a Limit
		// sample's dry, ceiling and enable — read `n` samples later, when its stage hands it back.
		Late in {};
		// THE LIMIT STAGES' INPUTS THIS SAMPLE: the driven signal (L/R, or M/S) and its detector levels
		// over the ceiling (so each stage's own ceiling is 1, and a moving Ceiling travels with the audio).
		float aA = 0.0f, aB = 0.0f, pA = 0.0f, pB = 0.0f;
		// **DISABLED AND LANDED IS A PURE PASS-THROUGH, DECIDED PER SAMPLE.** From the sample after the
		// bypass ramp lands at 0, no sample is touched, the detector is cleared and every other ramp is
		// frozen where it stood. It was once decided per BLOCK, so a ramp landing mid-block left the
		// rest of that block tracking and whether the detector was cleared before a re-enable depended
		// on where the host cut — test "a disable/re-enable cycle does not care how the host cuts the
		// block". The readings still go out (GR 0), so the card's meter keeps telling the truth.
		// **IT IS STILL LATE BY THE LATENCY**: the host was told one, and pads everything else by it.
		if (!state.enabled && bypass.settled() && bypass.value() == 0.0f) {
			clearDetector();
			in = { xL, xR, 1.0f, 0.0f, Kind::Pass };
			gr.push(0.0f);
			grA.push(0.0f);
			grB.push(0.0f);
		}
		else {
			// THE DETECTOR'S COPY — high-passed if asked; the audio is never filtered.
			// **THE DETECTOR'S COPY IS CLAMPED TO +-1e4 (+80 dBFS) BEFORE IT IS FILTERED OR SQUARED.** Nothing
			// real gets near it, and it bounds what one absurd sample can do: unclamped, a 1e30 sample rang the
			// SC HPF at 1e19..1e30 for ~100 ms and fed a mean square of up to 3e38, measured 22.6 dB off course
			// a second later. The audio path is not clamped — only what the detector hears.
			// The double biquad relies on the plugin's DenormalGuard (FTZ/DAZ) for its decaying state.
			// **DETECTOR: SIDE (backlog JO)** hears the plugin's sidechain at this sample in place of the input; nothing
			// connected (null, or a block shorter than the call) is silence, so it compresses nothing. The audio (`xL`, `xR`,
			// the dry, the wet) is the input either way.
			const bool haveSide = side && scL != nullptr && scR != nullptr && i < scFrames;
			const float detInL = side ? (haveSide ? finiteOr0(scL[i]) : 0.0f) : xL;
			const float detInR = side ? (haveSide ? finiteOr0(scR[i]) : 0.0f) : xR;
			const float hearL = std::clamp(detInL, -kDetectorClamp, kDetectorClamp);
			const float hearR = std::clamp(detInR, -kDetectorClamp, kDetectorClamp);
			float sL = hp ? static_cast<float>(hpL.step(static_cast<double>(hearL))) : hearL;
			float sR = hp ? static_cast<float>(hpR.step(static_cast<double>(hearR))) : hearR;
			// A FILTER THAT HAS GONE NON-FINITE (a huge finite input can overflow it) is cleared, not kept.
			if (!std::isfinite(sL)) {
				hpL.clear();
				sL = 0.0f;
			}
			if (!std::isfinite(sR)) {
				hpR.clear();
				sR = 0.0f;
			}
			// **THE TWO CHANNELS THE DETECTORS AND THE GAINS WORK ON (backlog LQ)**: L and R, or in M/S
			// `M = (L + R) / 2` and `S = (L - R) / 2`. The detector copies are encoded AFTER the sidechain
			// high-pass, which is linear, so filtering L and R and then encoding is filtering M and S — and
			// the filters keep one state whichever way the switch is set.
			if (ms) {
				const float m = (sL + sR) * 0.5f;
				const float s = (sL - sR) * 0.5f;
				sL = m;
				sR = s;
			}
			const float k = link.next();
			// THE ENABLE, RAMPED IN AND OUT: at 1 the device's output exactly, at 0 the input exactly — in
			// the same two-ended form as Mix, because the lerp form re-rounded every enabled sample (the Mix
			// 1 case caught it: `x + 1*(y - x)` is not y). Compress applies it here; Limit carries it with
			// the sample and applies it when the sample leaves.
			const float b = bypass.next();
			// **A MODE SWITCH IS A HARD CUT, CHOSEN**, and it is made HERE, on the input's timeline: every
			// sample leaves by the road it entered on, so the ones in flight at a flip are neither replayed
			// nor dropped. Mode is a stepped choice and not modulatable, so it moves only by a hand; a
			// crossfade between two gain computers would cost a second detector for a click nobody
			// automates. (The L/R-M/S switch is the same kind of choice and cuts the same way.)
			// **DRIVE, IN BOTH MODES** (backlog MI: drive knob for compressor): one gain in front
			// of the gain computer, so the detector and the audio it gains are pushed alike — Limit's drive since LE,
			// and Compress's now. The dry (Mix, the enable) stays the undriven input. At 0 dB it is `x * 1.0f`, x.
			const float dDb = driveDb.next();
			if (dDb != driveKeyDb) {
				driveKeyDb = dDb;
				driveLin = dbToLin(dDb);
			}
			const float d = driveLin;
			const float dL = xL * d, dR = xR * d;
			sL *= d;
			sR *= d;
			if (limit) {
				std::array<float, 2> xDb {};
				detectorDb = linkedLevelsDb(std::fabs(sL), std::fabs(sR), k, xDb);
				const float ceil = ceilingDb.next();
				if (ceil != ceilingKeyDb) {
					ceilingKeyDb = ceil;
					ceilingLin = dbToLin(ceil);
				}
				// EACH CHANNEL'S LEVEL OVER THE CEILING, linear: the stage limits it to 1, so the audio it
				// gains leaves at most at this sample's own ceiling.
				pA = dbToLin(xDb[0] - ceil);
				pB = xDb[1] == xDb[0] ? pA : dbToLin(xDb[1] - ceil);
				if (!ms) {
					aA = dL;
					aB = dR;
				}
				else {
					aA = (dL + dR) * 0.5f;
					aB = (dL - dR) * 0.5f;
				}
				in = { xL, xR, ceilingLin, b, ms ? Kind::LimitMidSide : Kind::Limit };
			}
			else {
				float lvlA;
				float lvlB;
				if (rms) {
					meanSquareA = settle(pRms * meanSquareA + (1.0f - pRms) * (sL * sL));
					meanSquareB = settle(pRms * meanSquareB + (1.0f - pRms) * (sR * sR));
					lvlA = std::sqrt(meanSquareA);
					lvlB = std::sqrt(meanSquareB);
				}
				else {
					lvlA = std::fabs(sL);
					lvlB = std::fabs(sR);
				}
				std::array<float, 2> xDb {};
				detectorDb = linkedLevelsDb(lvlA, lvlB, k, xDb);
				for (size_t ch = 0; ch < 2; ++ch) {
					const float target =
					    staticCurveDb(xDb[ch], state.thresholdDb, state.ratio, state.kneeDb) - xDb[ch];
					// THE BRANCHING SMOOTHER: attack while reduction is deepening, release while it lets go.
					const float c = target < gainDb[ch] ? pAttack : pRelease;
					gainDb[ch] = settle(c * gainDb[ch] + (1.0f - c) * target);
				}
				const float mk = makeupDb.next();
				const float gA = dbToLin(gainDb[0] + mk);
				const float gB = gainDb[1] == gainDb[0] ? gA : dbToLin(gainDb[1] + mk);
				const float m = mix.next();
				// **`(1-m)*x + m*wet` IS EXACT AT BOTH ENDS**: at m == 0 it is 1*x + 0*finite = x (the design:
				// "0 is exactly the dry signal"), at m == 1 it is 0*x + wet = wet. The lerp form `x + m*(wet - x)`
				// is exact only at 0 — at 1 it rounds `x + (wet - x)`.
				lastGain = gA;
				float wetL;
				float wetR;
				if (!ms) {
					wetL = dL * gA;
					wetR = dR * gB;
				}
				else {
					const float mWet = (dL + dR) * 0.5f * gA;
					const float sWet = (dL - dR) * 0.5f * gB;
					wetL = mWet + sWet;
					wetR = mWet - sWet;
				}
				const float yL = (1.0f - m) * xL + m * wetL;
				const float yR = (1.0f - m) * xR + m * wetR;
				// AND NOTHING NON-FINITE LEAVES: a huge finite input times a makeup gain can still overflow.
				in = { finiteOr0((1.0f - b) * xL + b * yL), finiteOr0((1.0f - b) * xR + b * yR), 1.0f, 0.0f,
					   Kind::Pass };
				// A POSITIVE NUMBER OF dB, scaled by the enable so a bypassed device reads what it does. The
				// headline is the deeper channel; each channel's own goes out beside it.
				const float redA = -gainDb[0];
				const float redB = -gainDb[1];
				gr.push(std::max(0.0f, std::max(redA, redB) * b));
				grA.push(std::max(0.0f, redA * b));
				grB.push(std::max(0.0f, redB * b));
			}
		}
		const std::array<float, 2> y = emit(in, aA, aB, pA, pB);
		left[i] = y[0];
		right[i] = y[1];
	}
	// **COMPRESS AND LIMIT WAIT FOR THE COMPANDER'S LANE** (backlog QG): its latency is the device's in every mode, so a
	// Mode flip never moves it. Zero while no lane holds anything, and then this does nothing at all.
	outPad.process(left, right, frames, lane.latency);
	// THE POINTERS WERE VALID FOR THIS CALL ONLY.
	scL = scR = nullptr;
	scFrames = 0;
	publish();
}

std::array<float, 2> CompDsp::emit(const Late& in, float aA, float aB, float pA, float pB) {
	const size_t n = late.size();
	const size_t msLate = msLim.lookahead();
	// THE LIMIT STAGES. L/R: one, the whole lookahead. M/S: Mid and Side for `msLate` samples, decoded,
	// then the linked L/R stage for the rest — each of Mid and Side at the ceiling decodes to up to twice
	// it, and the L/R stage takes the excess back AS A GAIN, looking ahead too, so the clamp below stays
	// the float-edge and SC-HPF guard it is in L/R rather than becoming a clipper in M/S.
	const std::array<float, 2> lrOut = step(lrLim, lrBusy, in.kind == Kind::Limit, aA, aB, pA, pB, true);
	const std::array<float, 2> msOut =
	    step(msLim, msBusy, in.kind == Kind::LimitMidSide, aA, aB, pA, pB, true);
	// WHAT LEAVES MID/SIDE NOW ENTERED `msLate` samples ago; its record says whether it is to be heard,
	// and at which ceiling. The L/R stage takes it decoded and over that ceiling, so its own is 1.
	const Late& half = late[(pos + n - msLate) % n];
	const bool halfReal = half.kind == Kind::LimitMidSide;
	const float inv = halfReal ? 1.0f / half.ceiling : 0.0f;
	const std::array<float, 2> stOut = step(stageLim, stageBusy, halfReal, (msOut[0] + msOut[1]) * inv,
	                                        (msOut[0] - msOut[1]) * inv, 0.0f, 0.0f, false);
	// WHAT LEAVES NOW: the sample that entered `n` ago, by the road it entered on.
	const Late out = late[pos];
	late[pos] = in;
	pos = pos + 1 == n ? 0 : pos + 1;
	float yL = out.a;
	float yR = out.b;
	if (out.kind != Kind::Pass) {
		float wL;
		float wR;
		if (out.kind == Kind::Limit) {
			wL = lrOut[0];
			wR = lrOut[1];
		}
		else {
			wL = stOut[0] * out.ceiling;
			wR = stOut[1] * out.ceiling;
		}
		// **THE CLAMP IS PART OF THE DESIGN, not a float-edge nicety**: with the SC HPF on, the detector
		// cannot hear bass the audio still carries, and this is what holds the ceiling — the ceiling this
		// sample entered with.
		const float c = out.ceiling;
		// THE STAGES' OWN PEAK OVER THE CEILING, before the clamp — the test seam that tells a ceiling held by a
		// GAIN from one held by the clamp. Nothing in the DSP reads it.
		peakOverCeiling = std::max(peakOverCeiling, std::max(std::fabs(wL), std::fabs(wR)) / c);
		wL = std::clamp(wL, -c, c);
		wR = std::clamp(wR, -c, c);
		yL = finiteOr0((1.0f - out.enable) * out.a + out.enable * wL);
		yR = finiteOr0((1.0f - out.enable) * out.b + out.enable * wR);
	}
	outPeak.push(std::max(std::fabs(yL), std::fabs(yR)));
	if (in.kind != Kind::Pass) {
		// LIMIT'S GAIN REDUCTION, as the stages applied it to their last samples out — a meter, so the
		// part of a sample's lookahead between the stages does not matter; the test seams read these.
		// In L/R `stageDb` is 0. Scaled by the enable, as Compress's are.
		const bool lr = in.kind == Kind::Limit;
		const dsp::LookaheadLimiter& first = lr ? lrLim : msLim;
		gainDb[0] = linToDb(first.gainForTest(0));
		gainDb[1] = linToDb(first.gainForTest(1));
		stageDb = lr ? 0.0f : linToDb(stageLim.gainForTest(0));
		const float redA = -(gainDb[0] + stageDb);
		const float redB = -(gainDb[1] + stageDb);
		gr.push(std::max(0.0f, std::max(redA, redB) * in.enable));
		grA.push(std::max(0.0f, redA * in.enable));
		grB.push(std::max(0.0f, redB * in.enable));
	}
	else {
		// THE L/R STAGE EXISTS ONLY IN M/S LIMIT, so it never carries a stale gain into the next switch.
		stageDb = 0.0f;
	}
	return { yL, yR };
}

//------------------------------------------------------------------------------------------------//
// THE COMPANDER (backlog QG)                                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE FOLLOWER'S INPUT FOR EACH CHANNEL: at Link 100% the louder channel's linear level for both — Buckets' own linked
/// detector (`max(|l|, |r|)`), to the bit — and below it each channel's dB blend (the device's Link) back in linear.
std::array<float, 2> followerLevels(float lvlA, float lvlB, float k, const std::array<float, 2>& xDb) {
	const float shared = std::max(finiteOr0(lvlA), finiteOr0(lvlB));
	return k >= 1.0f ? std::array<float, 2> { shared, shared } : std::array<float, 2> { dbToLin(xDb[0]), dbToLin(xDb[1]) };
}

/// ONE HALF'S GAIN ON ONE CHANNEL PAIR: (a, b) are L and R, or M and S; the gains are applied there and the pair decoded
/// back to L and R when `ms`.
std::array<float, 2> applyPair(float a, float b, const std::array<float, 2>& g, bool ms) {
	if (!ms) {
		return { a * g[0], b * g[1] };
	}
	const float m = (a + b) * 0.5f * g[0];
	const float s = (a - b) * 0.5f * g[1];
	return { m + s, m - s };
}

/// THE EXPONENT OF THE FIRST HALF AT RATIO R, applied to the envelope of what it HEARS (its input, or the sidechain): a
/// compressor's `1/R - 1`, an expander's `R - 1`.
float exponentFor(bool expands, float ratio) {
	return expands ? std::max(ratio, 1.0f) - 1.0f : dsp::compander::compressExponent(ratio);
}

} // namespace

// **WHY THE SECOND HALF LISTENS TO ITS OWN OUTPUT (the detector asymmetry, measured and designed out).** A first draft
// had both halves feed-forward — the second half's follower on the lane's output, the first half's on the input — and
// with an EMPTY lane the pair was not an identity on anything but DC: MEASURED on level-stepped noise, 2:1, 10/100 ms, a
// mean net gain error of 2.4 dB and a worst of 7.5 dB (Compander) and 8.9 dB (Excomper), because the second follower
// heard `g1 * x` and a follower of a product is not the product of followers.
//
// THE FIX IS THE NE570's OWN ARRANGEMENT, TURNED ROUND: the second half follows its OWN OUTPUT — the reconstruction of
// what the first half heard — so with an empty lane both followers are fed the same samples. Its exponent is the first
// half's NEGATED, so its static curve is the inverse (a feedback compressor with exponent `1 - R` is R:1; a feedback
// expander with `1 - 1/R` is 1:R — the doc has the algebra), and the range clamp is symmetric. For the two to cancel
// SAMPLE BY SAMPLE the first half's gain is taken from its envelope BEFORE this sample updates it, as a feedback stage's
// must be: then by induction both envelopes are the same number every sample, and `g1 * g2 = 1` to rounding.
//
// **A FEEDBACK STAGE IS STABLE ONLY WHILE ITS FOLLOWER IS SLOWER THAN ITS LOOP GAIN**: linearised in log, a step moves the
// error by `1 - a * (1 - e)` — always inside (0, 1) for the expander (Compander's second half), but for the compressor
// (Excomper's) `1 - a * R`, which oscillates past `a = 2 / R`. So in Excomper both followers' steps are capped at `1 / R`
// (both, so they stay equal): "instant" there means about R samples. Nothing else is capped.

void CompDsp::processCompander(float* left, float* right, int frames) {
	// THE OTHER MODES' OUTPUT PAD IS NOT IN USE HERE (see `process`).
	outPad.reset();
	const bool rms = state.detect == static_cast<int>(Detect::Rms);
	const bool hp = hpHz > 0.0f;
	const bool side = state.detector == static_cast<int>(Detector::Side);
	const bool ms = state.stereoMode == static_cast<int>(StereoMode::MidSide);
	const int topology = state.topology;
	const bool paired = isPaired(topology);
	const bool firstUp = firstExpands(topology);
	const bool linked = state.cmpLink;
	const bool haveLane = lane.process != nullptr;

	const auto aimRange = [this](float rangeDb) {
		if (rangeDb != rangeKeyDb) {
			rangeKeyDb = rangeDb;
			rangeHi = dbToLin(rangeDb);
			rangeLo = 1.0f / rangeHi;
		}
	};
	// THE DETECTOR'S COPY OF A PAIR: clamped, high-passed if asked (each half its own filters), M/S-encoded if asked.
	const auto detectorCopy = [&](float inL, float inR, dyn::Biquad& fL, dyn::Biquad& fR) {
		const float hearL = std::clamp(inL, -kDetectorClamp, kDetectorClamp);
		const float hearR = std::clamp(inR, -kDetectorClamp, kDetectorClamp);
		float sL = hp ? static_cast<float>(fL.step(static_cast<double>(hearL))) : hearL;
		float sR = hp ? static_cast<float>(fR.step(static_cast<double>(hearR))) : hearR;
		if (!std::isfinite(sL)) {
			fL.clear();
			sL = 0.0f;
		}
		if (!std::isfinite(sR)) {
			fR.clear();
			sR = 0.0f;
		}
		if (ms) {
			return std::array<float, 2> { (sL + sR) * 0.5f, (sL - sR) * 0.5f };
		}
		return std::array<float, 2> { sL, sR };
	};
	// ONE HALF'S DETECTOR STEP: the copy squared or rectified, Link-blended, and the follower moved — the law both halves
	// share, on each half's own state.
	const auto detect = [&](std::array<float, 2> s, float k, float& msA, float& msB, std::array<float, 2>& follower,
	                        float att, float rel, bool report) {
		float lvlA;
		float lvlB;
		if (rms) {
			msA = settle(pRms * msA + (1.0f - pRms) * (s[0] * s[0]));
			msB = settle(pRms * msB + (1.0f - pRms) * (s[1] * s[1]));
			lvlA = std::sqrt(msA);
			lvlB = std::sqrt(msB);
		}
		else {
			lvlA = std::fabs(s[0]);
			lvlB = std::fabs(s[1]);
		}
		std::array<float, 2> xDb {};
		const float sharedDb = linkedLevelsDb(lvlA, lvlB, k, xDb);
		if (report) {
			detectorDb = sharedDb;
		}
		const std::array<float, 2> lvl = followerLevels(lvlA, lvlB, k, xDb);
		for (size_t ch = 0; ch < 2; ++ch) {
			dsp::compander::follow(follower[ch], std::min(lvl[ch], kDetectorClamp), att, rel, kEnvSilence);
		}
	};
	const auto gains = [&](const std::array<float, 2>& follower, float exponent, std::array<float, 2>& dbOut) {
		std::array<float, 2> g {};
		for (size_t ch = 0; ch < 2; ++ch) {
			g[ch] = dsp::compander::gain(follower[ch], refLin, exponent, rangeLo, rangeHi);
			dbOut[ch] = 20.0f * std::log10(g[ch]);
		}
		return g;
	};

	for (int at = 0; at < frames; at += kChunk) {
		const int n = std::min(kChunk, frames - at);
		float* const L = left + at;
		float* const R = right + at;
		// **DISABLED AND LANDED** at the chunk's start: the lane is not run, the halves forget, the dry passes — still
		// late by the lane's latency, as the host was told (`dryPad`), and by the device's own (`emit`).
		const bool off = !state.enabled && bypass.settled() && bypass.value() == 0.0f;

		// PASS 1 — THE FIRST HALF, on the input, into `wet`; the dry kept beside it.
		for (int i = 0; i < n; ++i) {
			const size_t u = static_cast<size_t>(i);
			const float xL = std::isfinite(L[i]) ? L[i] : 0.0f;
			const float xR = std::isfinite(R[i]) ? R[i] : 0.0f;
			inPeak.push(std::max(std::fabs(xL), std::fabs(xR)));
			dryL[u] = xL;
			dryR[u] = xR;
			const float k = link.next();
			const float ratio = cmpRatio.next();
			const float rangeDb = cmpRange.next();
			kAt[u] = k;
			ratioAt[u] = ratio;
			rangeAt[u] = rangeDb;
			if (off) {
				clearDetector();
				wetL[u] = xL;
				wetR[u] = xR;
				continue;
			}
			const int sample = at + i;
			const bool haveSide = side && scL != nullptr && scR != nullptr && sample < scFrames;
			const float detInL = side ? (haveSide ? finiteOr0(scL[sample]) : 0.0f) : xL;
			const float detInR = side ? (haveSide ? finiteOr0(scR[sample]) : 0.0f) : xR;
			const float dDb = driveDb.next();
			if (dDb != driveKeyDb) {
				driveKeyDb = dDb;
				driveLin = dbToLin(dDb);
			}
			const float d = driveLin;
			aimRange(rangeDb);
			// EXCOMPER'S STABILITY CAP (the banner): both followers at most 1/R a step.
			const float cap = topology == static_cast<int>(Topology::Excomper)
			                      ? 1.0f / std::max({ ratio, linked ? ratio : state.cmpRatio2, 1.0f })
			                      : 1.0f;
			// THE GAIN FIRST, FROM THE ENVELOPE AS IT STOOD — then this sample moves it (the banner: the second half's
			// feedback has no other way to match it).
			const std::array<float, 2> g = gains(env, exponentFor(firstUp, ratio), firstDb);
			std::array<float, 2> s = detectorCopy(detInL, detInR, hpL, hpR);
			s[0] *= d;
			s[1] *= d;
			detect(s, k, meanSquareA, meanSquareB, env, std::min(cmpAttack, cap), std::min(cmpRelease, cap), true);
			const std::array<float, 2> w = applyPair(xL * d, xR * d, g, ms);
			wetL[u] = w[0];
			wetR[u] = w[1];
		}

		// THE LANE, over the first half's output — run only while it holds something and the device is not landed off.
		uint32_t late = 0;
		if (haveLane && !off) {
			lane.process(lane.context, wetL.data(), wetR.data(), n);
			late = lane.latency;
		}
		// THE DRY WAITS FOR THE LANE, so Mix and the enable blend two signals that left together.
		dryPad.process(dryL.data(), dryR.data(), n, late);

		// PASS 2 — THE SECOND HALF (paired) on what the lane handed back, then makeup, mix and the enable.
		for (int i = 0; i < n; ++i) {
			const size_t u = static_cast<size_t>(i);
			const float yL = finiteOr0(wetL[u]);
			const float yR = finiteOr0(wetR[u]);
			const float b = bypass.next();
			const float m = mix.next();
			const float mk = makeupDb.next();
			const float ratio2 = cmpRatio2.next();
			if (mk != makeupKeyDb) {
				makeupKeyDb = mk;
				makeupLin = dbToLin(mk);
			}
			float outL = yL;
			float outR = yR;
			secondDb = { 0.0f, 0.0f };
			if (paired && !off) {
				aimRange(rangeAt[u]);
				const float r2 = linked ? ratioAt[u] : ratio2;
				const float cap = topology == static_cast<int>(Topology::Excomper)
				                      ? 1.0f / std::max({ ratioAt[u], r2, 1.0f })
				                      : 1.0f;
				// THE FIRST HALF'S EXPONENT AT r2, NEGATED, on the envelope of this half's OWN OUTPUT (the banner).
				const std::array<float, 2> g = gains(env2, -exponentFor(firstUp, r2), secondDb);
				const std::array<float, 2> w = applyPair(yL, yR, g, ms);
				outL = w[0];
				outR = w[1];
				const std::array<float, 2> s = detectorCopy(outL, outR, hp2L, hp2R);
				detect(s, kAt[u], meanSquare2A, meanSquare2B, env2, std::min(cmpAttack2, cap), std::min(cmpRelease2, cap),
				       false);
			}
			// THE NET GAIN EACH CHANNEL TOOK, signed dB — what the meter and the seams read.
			gainDb = { firstDb[0] + secondDb[0], firstDb[1] + secondDb[1] };
			lastGain = dbToLin(gainDb[0]) * makeupLin;
			const float wL = outL * makeupLin;
			const float wR = outR * makeupLin;
			const float dL = dryL[u];
			const float dR = dryR[u];
			// MIX AND THE ENABLE IN THE TWO-ENDED FORM (Compress's): exact at both ends.
			const float mixL = (1.0f - m) * dL + m * wL;
			const float mixR = (1.0f - m) * dR + m * wR;
			const Late in { finiteOr0((1.0f - b) * dL + b * mixL), finiteOr0((1.0f - b) * dR + b * mixR), 1.0f, 0.0f,
				            Kind::Pass };
			const float redA = off ? 0.0f : -gainDb[0];
			const float redB = off ? 0.0f : -gainDb[1];
			gr.push(std::max(0.0f, std::max(redA, redB) * b));
			grA.push(std::max(0.0f, redA * b));
			grB.push(std::max(0.0f, redB * b));
			const std::array<float, 2> y = emit(in, 0.0f, 0.0f, 0.0f, 0.0f);
			L[i] = y[0];
			R[i] = y[1];
		}
	}
	scL = scR = nullptr;
	scFrames = 0;
	publish();
}

} // namespace feathers::fx::comp
