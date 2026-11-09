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

#include "fx/clip/clip_types.h"

#include "fx/dyn/biquad.h"
#include "fx/dyn/clip_curve.h"
#include "fx/dyn/meter_ramp.h"
#include "fx/dyn/oversampler.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <new>
#include <numbers>

// CLIP'S RENDER PATH — gain into the normalised domain, one of five curves (at 4x when asked), gain
// back out.
//
// **ONE HOST SAMPLE AT A TIME, THROUGH THE OVERSAMPLER TOO.** The ramps, the enable's pass-through
// decision, the Texture high-pass and the Grit PRNG all advance per sample, and `Oversampler4x` is
// per-sample with no carry — so a host that cuts a block anywhere gets the same bits (COMP's partition
// lesson, applied from the start).

namespace feathers::fx::clip {

namespace {

/// dB TO LINEAR, as `exp` rather than `pow(10, .)`.
[[nodiscard]] inline float dbToLin(float db) {
	constexpr float kLn10Over20 = 0.11512925464970229f;
	return std::exp(db * kLn10Over20);
}

/// A NON-FINITE VALUE BECOMES 0 — the one treatment every guard in the loop uses.
[[nodiscard]] inline float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

/// **THE NORMALISED SIGNAL IS CLAMPED TO +-1e4 BEFORE THE CURVES AND THE OVERSAMPLER SEE IT.** Every
/// curve is flat or folding far below that, so nothing audible changes; what it prevents is a huge
/// finite input (1e30 through +24 dB of drive over a -24 dB ceiling) overflowing the upsampler's state
/// to inf and latching it at NaN. COMP's detector clamp, for the same reason.
constexpr float kNormalisedClamp = 1e4f;

/// **THE REST: A TAIL THAT CANNOT REACH ZERO IS STOPPED AT A LEVEL NOTHING CAN HEAR.** The halfband
/// allpass tails decay geometrically into the denormals and PARK there (`oversampler.h` says nothing in it
/// flushes them), so after a burst the output never became exact silence — `test_fx_devices`'s tail case
/// named CLIP. MultimodeFilter's and OWLTT's `kRestLevel` / `kRestSilentSeconds` are the precedent. It may
/// only engage after `kRestSilentSeconds` of EXACTLY-zero input with every output sample under
/// `kRestLevel` (-180 dBFS), so it can never touch audible output: then the oversampler and the Texture
/// high-pass are cleared and the output is exact zero until the input moves again.
constexpr float kRestLevel = 1.0e-9f;
constexpr double kRestSilentSeconds = 0.05;

/// THE GRIT PRNG'S SEED — a constant, so a render is repeatable and `reset` means "as if just switched on". **NOT
/// SAVED WITH THE PATCH, AND WHY** (backlog NM): Grit is a per-sample texture; where its stream stands is not audible.
constexpr uint32_t kSeed = 0x9e3779b9u;

struct OpsClip {
	Settings state {};
	double sampleRate = 48000.0;

	/// THE OVERSAMPLER. Trivially copyable, but reset in place on a toggle rather than replaced.
	dyn::Oversampler4x os {};
	/// WHETHER THE LAST PROCESSED SAMPLE RAN AT 4x — what a toggle is detected against.
	bool osApplied = true;
	/// **THE DRY, LATE BY WHAT THE DEVICE DECLARES** (`kOversampleLatency` while 4x runs, none off; backlog NR): what the
	/// enable crossfade and the disabled pass-through read, so the whole device is late by the one count the host was
	/// told. The RAW input (a disabled device still passes a NaN as a NaN, six samples on). Cleared by `prepare` and
	/// `reset` only: the disabled landing keeps passing it, and dropping what it holds would cut six samples out.
	static constexpr size_t kDryLen = 8;
	std::array<float, kDryLen> dryL {}, dryR {};
	size_t dryWrite = 0;
	static_assert(kOversampleLatency < kDryLen, "the ring must hold the declared delay");
	void clearDry() {
		dryL.fill(0.0f);
		dryR.fill(0.0f);
		dryWrite = 0;
	}

	/// TEXTURE'S HIGH-PASS on what clipping removed, one per channel, **designed at the PROCESSING rate**:
	/// 4 fs with oversampling on, fs with it off. A corner designed at fs and run at 4 fs would sit two
	/// octaves high.
	dyn::Biquad hpL {};
	dyn::Biquad hpR {};
	double hpHz = 0.0;
	double hpRate = 0.0;

	/// THE GRIT NOISE: xorshift32, seeded to `kSeed` in `reset`, one draw per channel per PROCESSED
	/// (oversampled, when on) sample in Grit mode — so the draw order depends on the sample count, never on
	/// where a block was cut.
	uint32_t rng = kSeed;

	dyn::LinearRamp driveDb, ceilingDb, outputDb, bypass;
	/// **THE SHAPE CONTROLS ARE RAMPED TOO, NOT ONLY THE GAINS.** Knee, Foldback, Grit and Depth are
	/// modulatable and `setFields` lands once a block, so unramped they moved in block-sized steps — a
	/// zipper, and Depth is effectively a gain on the folded part (`foldCurve` mixes clamp and fold by it).
	/// Advanced once per HOST sample (so the four oversampled frames of one sample share a value, and the
	/// cut of a block changes nothing) and read by `shape` through `kneeNow` and its siblings.
	dyn::LinearRamp knee, foldback, grit, depth;
	float kneeNow = 0.1f, foldbackNow = 0.5f, gritNow = 0.5f, depthNow = 1.0f;
	/// THE MODE THE LAST BLOCK RAN IN — what "the mode changed into Texture" is detected against.
	int modeApplied = 0;
	dyn::HeldPeak inPeak, outPeak, over;

	/// dB2lin of the last drive, ceiling and output, keyed on the dB value (a settled ramp repeats it).
	float driveKey = 0.0f, driveLin = 1.0f;
	float ceilingKey = 0.0f, ceilingLin = 1.0f;
	float outputKey = 0.0f, outputLin = 1.0f;

	/// TRUE ONCE THE DISABLED PASS-THROUGH HAS CLEARED THE STATE, so it is cleared once per landing.
	bool idle = false;

	/// THE REST (`kRestLevel`): consecutive exactly-zero input samples, the count that means "long enough"
	/// at this rate, and whether the tails have been cleared for this stretch of silence.
	int silentRun = 0;
	int restSamples = 2400;
	bool rested = false;

	device::ReadingsSink sink {};

	[[nodiscard]] double processingRate() const { return state.oversample ? 4.0 * sampleRate : sampleRate; }

	/// THE HIGH-PASS IS A FUNCTION OF Freq AND THE PROCESSING RATE; redesigned when either moves, state kept
	/// (a moving corner must not silence the carve for a block).
	///
	/// **THE CORNER IS CLAMPED TO 0.45 OF THE PROCESSING RATE.** Freq reaches 12 kHz, which is past Nyquist
	/// at 22.05 kHz with oversampling off; the RBJ design there has w0 > pi and its poles leave the unit
	/// circle. Just under Nyquist the filter is stable and still a high-pass on the top of the band.
	void designHighpass() {
		const double rate = processingRate();
		const double hz = std::min(std::exp2(static_cast<double>(state.freq)), 0.45 * rate);
		if (hz != hpHz || rate != hpRate) {
			hpL.designHighpass(hz, rate);
			hpR.designHighpass(hz, rate);
			hpHz = hz;
			hpRate = rate;
		}
	}

	void aimRamps() {
		driveDb.set(state.driveDb);
		ceilingDb.set(state.ceilingDb);
		outputDb.set(state.outputDb);
		bypass.set(state.enabled ? 1.0f : 0.0f);
		knee.set(state.knee);
		foldback.set(state.foldback);
		grit.set(state.grit);
		depth.set(state.depth);
	}

	/// EVERYTHING WITH MEMORY FORGETS IT: "as if just switched on".
	void clearState() {
		os.reset();
		hpL.clear();
		hpR.clear();
		rng = kSeed;
		silentRun = 0;
		rested = false;
	}

	[[nodiscard]] float noise01() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
	}

	/// ONE CHANNEL'S TEXTURE DETAIL — HP(u - c), and a filter that has gone non-finite is cleared.
	[[nodiscard]] static float detail(dyn::Biquad& hp, float removed) {
		const double d = hp.step(static_cast<double>(removed));
		if (!std::isfinite(d)) {
			hp.clear();
			return 0.0f;
		}
		return static_cast<float>(d);
	}

	/// THE CURVE, on one frame (host rate, or each of the four oversampled frames). In place.
	void shape(float& l, float& r) {
		const float knee = kneeNow;
		switch (static_cast<Mode>(state.mode)) {
		case Mode::Soft:
			l = dyn::softClip(l, knee);
			r = dyn::softClip(r, knee);
			break;
		case Mode::Texture: {
			const float cl = dyn::softClip(l, knee);
			const float cr = dyn::softClip(r, knee);
			l = textureCurve(cl, detail(hpL, l - cl), foldbackNow);
			r = textureCurve(cr, detail(hpR, r - cr), foldbackNow);
			break;
		}
		case Mode::Grit: {
			// ONE DRAW PER CHANNEL, left then right, on every processed frame.
			const float nl = noise01();
			const float nr = noise01();
			l = gritCurve(dyn::softClip(l, knee), l, knee, gritNow, nl);
			r = gritCurve(dyn::softClip(r, knee), r, knee, gritNow, nr);
			break;
		}
		case Mode::Fold:
			l = foldCurve(l, depthNow);
			r = foldCurve(r, depthNow);
			break;
		case Mode::Bits:
			l = bitsCurve(l, knee, state.bits + 1);
			r = bitsCurve(r, knee, state.bits + 1);
			break;
		case Mode::kCount:
			break;
		}
	}

	void publish() {
		if (sink.publish != nullptr) {
			const float values[static_cast<size_t>(Reading::kCount)] = { inPeak.reading(), outPeak.reading(),
				                                                         over.reading() };
			sink.publish(sink.context, values, static_cast<size_t>(Reading::kCount));
		}
	}

	void process(float* left, float* right, int frames) {
		aimRamps();
		// **TOGGLING OVERSAMPLE RESETS THE OVERSAMPLER AND REDESIGNS THE HIGH-PASS, AND MAY CLICK.** The two
		// paths have different group delays and the halfband state belongs to one of them; a crossfade
		// between them is a refinement (the design accepts a click on a quality switch). Checked per block
		// because Oversample moves only through `setFields`, which lands between blocks.
		if (state.oversample != osApplied) {
			os.reset();
			hpL.clear();
			hpR.clear();
			osApplied = state.oversample;
			designHighpass();
		}
		// **INTO TEXTURE, THE HIGH-PASS STARTS EMPTY.** No other mode steps it, so outside Texture its state
		// is whatever the last Texture stretch left — and carving that stale detail into the first samples
		// after a switch back is a click the new signal did not make. Mode moves only through `setFields`,
		// so between blocks, which is where this looks.
		if (state.mode != modeApplied) {
			if (state.mode == static_cast<int>(Mode::Texture)) {
				hpL.clear();
				hpR.clear();
			}
			modeApplied = state.mode;
		}
		const size_t delay = osApplied ? kOversampleLatency : 0;
		for (int i = 0; i < frames; ++i) {
			// A NON-FINITE INPUT SAMPLE IS TREATED AS 0, before anything with memory sees it (COMP's lesson).
			const float xL = std::isfinite(left[i]) ? left[i] : 0.0f;
			const float xR = std::isfinite(right[i]) ? right[i] : 0.0f;
			// THE DRY AT THE DECLARED LATENCY: with no delay the read is the sample just written, so 1x is unchanged.
			dryL[dryWrite] = left[i];
			dryR[dryWrite] = right[i];
			const size_t dryRead = (dryWrite + kDryLen - delay) % kDryLen;
			dryWrite = (dryWrite + 1) % kDryLen;
			const float rawL = dryL[dryRead];
			const float rawR = dryR[dryRead];
			const float dL = std::isfinite(rawL) ? rawL : 0.0f;
			const float dR = std::isfinite(rawR) ? rawR : 0.0f;
			// **DISABLED AND LANDED IS A PURE PASS-THROUGH, DECIDED PER SAMPLE** — COMP's partition fix. No
			// sample is touched (a NaN passes as a NaN: an off device changes nothing), the state is cleared
			// once, and the ramps are frozen because nothing calls `next()`.
			if (!state.enabled && bypass.settled() && bypass.value() == 0.0f) {
				if (!idle) {
					clearState();
					idle = true;
				}
				// THE PASS-THROUGH IS LATE BY THE DECLARED COUNT TOO, raw (a bypass still delays: the latency contract).
				left[i] = rawL;
				right[i] = rawR;
				inPeak.push(std::max(std::fabs(xL), std::fabs(xR)));
				outPeak.push(std::max(std::fabs(dL), std::fabs(dR)));
				over.push(0.0f);
				continue;
			}
			idle = false;
			const float dDb = driveDb.next();
			if (dDb != driveKey) {
				driveKey = dDb;
				driveLin = dbToLin(dDb);
			}
			const float cDb = ceilingDb.next();
			if (cDb != ceilingKey) {
				ceilingKey = cDb;
				ceilingLin = dbToLin(cDb);
			}
			const float oDb = outputDb.next();
			if (oDb != outputKey) {
				outputKey = oDb;
				outputLin = dbToLin(oDb);
			}
			kneeNow = knee.next();
			foldbackNow = foldback.next();
			gritNow = grit.next();
			depthNow = depth.next();
			const float onsetU = onset(state.mode, kneeNow);
			const float gain = driveLin / ceilingLin;
			float uL = std::clamp(xL * gain, -kNormalisedClamp, kNormalisedClamp);
			float uR = std::clamp(xR * gain, -kNormalisedClamp, kNormalisedClamp);
			// THE IN METER READS THE INCOMING SIGNAL, BEFORE DRIVE — the same quantity the disabled branch pushes,
			// so the bar holds still across a toggle. The Over light is metered AHEAD of the curve, on the driven
			// signal, so it lights exactly when the curve has something to do (Bus Utility's rule).
			inPeak.push(std::max(std::fabs(xL), std::fabs(xR)));
			// **WITH 4x THE CURVE RUNS ON THE OVERSAMPLED FRAMES, SO THAT IS WHERE THE LIGHT LOOKS.** An
			// inter-sample peak can cross the onset while no host sample does, and the curve acts on it; a
			// host-rate check would leave the light dark while the clipper worked. Off, the host samples ARE
			// what the curve sees.
			float peakU = 0.0f;
			// **WITH 4x THE CEILING HOLDS AT THE OVERSAMPLED RATE, NOT AT HOST RATE** — the decision
			// (2026-09-25): documented, no safety clamp; most oversampling clippers behave this way. The
			// curve's ±1 is exact on every 4x frame, but the clipped 4x signal is nearly square, and the
			// polyphase IIR halfband decimator is not linear-phase, so band-limiting it rebuilds peaks
			// between the clipped samples. MEASURED (Drive +24, Ceiling -6, worst mode per signal, Soft and
			// Bits; re-measured 2026-09-25 on the clean-room halfbands): 50 Hz square 1.45x the ceiling,
			// 1 kHz sine 1.42x, random full-scale bursts 2.73x; Texture 1.05x, Fold 1.11x.
			// `test_fx_clip.cpp`'s ceiling case asserts these as regression bounds; with Oversample off the
			// ceiling is exact and asserted strictly.
			if (state.oversample) {
				os.process(&uL, &uR, 1, [this, &peakU](float& l, float& r) {
					peakU = std::max({ peakU, std::fabs(l), std::fabs(r) });
					shape(l, r);
				});
			}
			else {
				peakU = std::max(std::fabs(uL), std::fabs(uR));
				shape(uL, uR);
			}
			over.push(peakU > onsetU ? 1.0f : 0.0f);
			const float back = ceilingLin * outputLin;
			const float b = bypass.next();
			// THE ENABLE IN THE TWO-ENDED FORM (COMP's Mix lesson): exact dry at 0, exact y at 1.
			//
			// **THE DRY IS THE DECLARED-LATE ONE** (`kOversampleLatency` with 4x): before NR it was undelayed, and the
			// 5 ms crossfade summed two copies six samples apart. Now it is the slot's alignment, inside the device.
			float yL = finiteOr0((1.0f - b) * dL + b * (uL * back));
			float yR = finiteOr0((1.0f - b) * dR + b * (uR * back));
			// THE REST, per sample (so it is cut-independent) — see `kRestLevel`.
			if (xL == 0.0f && xR == 0.0f) {
				silentRun = std::min(silentRun + 1, restSamples);
			}
			else {
				silentRun = 0;
				rested = false;
			}
			if (silentRun >= restSamples && std::fabs(yL) < kRestLevel && std::fabs(yR) < kRestLevel) {
				if (!rested) {
					os.reset();
					hpL.clear();
					hpR.clear();
					rested = true;
				}
				yL = 0.0f;
				yR = 0.0f;
			}
			left[i] = yL;
			right[i] = yR;
			outPeak.push(std::max(std::fabs(yL), std::fabs(yR)));
		}
		publish();
	}
};

OpsClip& asClip(void* storage) { return *static_cast<OpsClip*>(storage); }

} // namespace

double textureHighpassGainForTest(const void* storage, double hz) {
	const OpsClip& ops = *static_cast<const OpsClip*>(storage);
	// |H(e^jw)| OF THE COEFFICIENTS INSTALLED, at the rate the device is RUNNING — computed here from
	// `processingRate()`, independently of whatever rate the design call was handed.
	const double w = 2.0 * std::numbers::pi_v<double> * hz / ops.processingRate();
	const std::complex<double> z1 = std::polar(1.0, -w);
	const std::complex<double> z2 = z1 * z1;
	const dyn::Biquad& b = ops.hpL;
	return std::abs((b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2));
}

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsClip),
    .stateAlign = alignof(OpsClip),
    .construct =
        [](void* storage) {
	        new (storage) OpsClip();
	        OpsClip& ops = asClip(storage);
	        ops.osApplied = ops.state.oversample;
	        ops.modeApplied = ops.state.mode;
	        ops.clearState();
	        ops.designHighpass();
        },
    .destruct = [](void* storage) { asClip(storage).~OpsClip(); },
    .prepare =
        [](void* storage, double sampleRate, int) {
	        OpsClip& ops = asClip(storage);
	        ops.sampleRate = std::max(sampleRate, 1.0);
	        ops.restSamples = std::max(1, static_cast<int>(std::lround(kRestSilentSeconds * ops.sampleRate)));
	        for (dyn::LinearRamp* r : { &ops.driveDb, &ops.ceilingDb, &ops.outputDb, &ops.bypass, &ops.knee,
		                                     &ops.foldback, &ops.grit, &ops.depth }) {
		        r->prepare(ops.sampleRate);
	        }
	        ops.inPeak.prepare(ops.sampleRate);
	        ops.outPeak.prepare(ops.sampleRate);
	        ops.over.prepare(ops.sampleRate);
	        ops.clearState();
	        ops.clearDry();
	        ops.designHighpass();
        },
    .reset =
        [](void* storage) {
	        OpsClip& ops = asClip(storage);
	        ops.clearState();
	        ops.idle = false;
	        ops.clearDry();
	        for (dyn::LinearRamp* r : { &ops.driveDb, &ops.ceilingDb, &ops.outputDb, &ops.bypass, &ops.knee,
		                                     &ops.foldback, &ops.grit, &ops.depth }) {
		        r->reset();
	        }
	        ops.inPeak.reset();
	        ops.outPeak.reset();
	        ops.over.reset();
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, so absent fields land on their DECLARED defaults.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        OpsClip& ops = asClip(storage);
	        ops.state = stateOf(row, take);
	        // AN OVERSAMPLE TOGGLE IS APPLIED BY `process` (it resets the oversampler); a Freq change is
	        // applied here. The design is keyed on the rate `processingRate()` names, so it is right either way.
	        if (ops.state.oversample == ops.osApplied) {
		        ops.designHighpass();
	        }
        },
    .processStereo = [](void* storage, float* left, float* right,
                        int frames) { asClip(storage).process(left, right, frames); },
    // NO MONO ROAD: stereo by nature on the bus; the host hands a mono buffer as two.
    .processMono = nullptr,
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asClip(storage).sink = sink; },
    .setOuterSources = nullptr,
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    // NO WINDOWED CONTROLS: the plain `setFields` road, with every continuous control ramped — the three
    // gains, the enable, and the four shape controls (`knee`, `foldback`, `grit`, `depth`).
    .windowedControls = {},
    .setFieldWindows = nullptr,
    // **4x IS A LATENCY (backlog NR)**: a pure function of the STORED Oversample (L1: it is not modulatable) and nothing
    // else, enabled or not, at any rate — the halfbands' delay is in host samples. An absent field is its default, 4x.
    .latencySamples = [](const void*, const float* fields, size_t fieldsPresent, double) -> uint32_t {
	    const auto at = static_cast<size_t>(Param::Oversample);
	    const float v = fields != nullptr && at < fieldsPresent ? fields[at] : static_cast<float>(kParamDescs[at].def);
	    return std::isfinite(v) && v >= 0.5f ? kOversampleLatency : 0u;
    },
};

} // namespace feathers::fx::clip
