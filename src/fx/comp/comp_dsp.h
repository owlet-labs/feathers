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
#pragma once

#include "fx/comp/comp_types.h"

#include "device/latency.h"
#include "dsp/compander.h"
#include "dsp/lookahead_limiter.h"
#include "fx/dyn/biquad.h"
#include "fx/dyn/meter_ramp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// COMP'S DSP, AS A TYPE (backlog QG): it was an anonymous struct in `comp.cpp` until the Compander's encode/decode lane
// needed a holder that can see the rack (`comp_ops.cpp`, in `feathers-fx-sklitter`, the Delay's arrangement). The DSP
// stays in `feathers-fx` — `process` and the compander's two-pass road are defined in `comp.cpp` — and the holder owns
// one `CompDsp` beside its `rack::LazyLane`, handing the DSP a `LaneInsert` each block.

// COMP'S RENDER PATH — a detector per channel (L/R or M/S, backlog LQ) blended by Stereo Link, a gain
// computer in dB, and the branching smoother (Giannoulis, Massberg & Reiss 2012) in the dB domain.
//
// **EVERYTHING IS PER SAMPLE AND CARRIED ACROSS BLOCKS**: the smoother, the RMS mean square, the ramps
// and the holds count samples, never blocks, so the host's buffer size is not audible (Bus Utility's
// `kRampSeconds` note records the partition defect that taught this).

namespace feathers::fx::comp {

namespace comp_detail {

/// dB TO LINEAR, as `exp` rather than `pow(10, .)`: one transcendental per call on the audio thread.
[[nodiscard]] inline float dbToLin(float db) {
	constexpr float kLn10Over20 = 0.11512925464970229f;
	return std::exp(db * kLn10Over20);
}

/// THE DETECTOR'S FLOOR: silence reads -180 dB rather than -inf, so the gain computer never sees one.
inline constexpr float kFloor = 1e-9f;
/// A LIMIT STAGE'S LINEAR GAIN IN dB, for the meter and the test seams; a gain of exactly 1 is exactly 0 dB.
[[nodiscard]] inline float linToDb(float g) {
	return g >= 1.0f ? 0.0f : 20.0f * std::log10(std::max(g, kFloor));
}
/// THE DETECTOR'S CEILING: +80 dBFS. See the clamp in `process`.
inline constexpr float kDetectorClamp = 1e4f;
/// THE COMPANDER FOLLOWER'S FLOOR: under it the envelope is exactly 0, so a tail ends in silence (Buckets' `kSilenceFloor`).
inline constexpr float kEnvSilence = 1.0e-7f;

/// A NON-FINITE VALUE BECOMES 0 — the one treatment every guard in the loop uses.
[[nodiscard]] inline float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

/// THE RECURSIVE STATE'S GUARD, applied on every write: non-finite resets to 0 (a latch would otherwise
/// be permanent), and anything under 1e-12 flushes to 0 so a long release or a silent mean square never
/// decays into denormals.
[[nodiscard]] inline float settle(float v) {
	return (std::isfinite(v) && std::fabs(v) >= 1e-12f) ? v : 0.0f;
}
} // namespace comp_detail

using namespace comp_detail;

struct CompDsp {
	Settings state {};
	double sampleRate = 48000.0;

	/// THE SIDECHAIN HIGH-PASS — on the DETECTOR's copy of each channel, never on the audio.
	dyn::Biquad hpL {};
	dyn::Biquad hpR {};
	/// THE CORNER AND RATE THE FILTER WAS LAST DESIGNED AT; 0 Hz is off (a true skip).
	float hpHz = 0.0f;
	double hpRate = 0.0;

	/// THE RMS DETECTOR'S MEAN SQUARES, ONE PER CHANNEL (linear power, not dB) — L and R, or M and S.
	/// **THE SHARED LEVEL IS THE LOUDER CHANNEL'S RMS**, `sqrt(max(msA, msB))` — the same as Peak's
	/// `max(|A|, |B|)`, and the one detector this device had before backlog LQ.
	float meanSquareA = 0.0f;
	float meanSquareB = 0.0f;
	/// THE SMOOTHED GAIN REDUCTION PER CHANNEL, in dB, <= 0 — [0] is L (or M), [1] is R (or S).
	///
	/// **AT LINK 100% THE TWO ARE THE SAME NUMBER BY CONSTRUCTION**: both channels' detectors read the shared
	/// level exactly (`(1 - 1) * own + 1 * shared` is `shared` to the bit), so both smoothers run one
	/// trajectory and each equals the one `gainDb` the device carried before LQ.
	std::array<float, 2> gainDb { 0.0f, 0.0f };
	/// M/S LIMIT'S L/R STAGE, in dB, <= 0. Each of Mid and Side is limited to the ceiling, and decoded they
	/// can sum to twice it; this takes the excess back as a GAIN (`stageLim`, looking ahead, the device's release)
	/// rather than leaving it to the clamp, which would clip it. 0 and untouched in L/R and in Compress.
	float stageDb = 0.0f;
	/// **WRITTEN ON PURPOSE FOR THE TEST SEAMS**, one store per sample: the detector's last level in dB
	/// (`detectorDbForTest`) and the linear gain Compress applied to the last sample, makeup included
	/// (`lastGainForTest`). Nothing in the DSP reads them.
	float detectorDb = -180.0f;
	float lastGain = 1.0f;
	/// THE LOUDEST LIMIT SAMPLE BEFORE THE FINAL CLAMP, over the ceiling it entered with (`peakOverCeilingForTest`):
	/// at most 1 while the stages hold the ceiling themselves. A running maximum since construct or `reset`.
	float peakOverCeiling = 0.0f;

	/// dB2lin OF THE LAST DRIVE AND CEILING, keyed on the dB value — a settled ramp returns the same
	/// value every sample, so this skips the `exp` whenever the control is not moving.
	float driveKeyDb = 0.0f, driveLin = 1.0f;
	float ceilingKeyDb = 0.0f, ceilingLin = 1.0f;

	float pAttack = 0.0f;
	float pRelease = 0.0f;
	float pRms = 0.0f;

	/// THE COMPANDER (backlog QG): one LINEAR follower per channel (L/R or M/S), the chip's capacitor; its attack and
	/// release steps (`dsp::compander::coefFor`, 1 when the control is "off": instant); Ref as a level; and the range's
	/// clamps and makeup as linear gains, keyed on their dB like Drive so a settled ramp skips the `exp`.
	std::array<float, 2> env { 0.0f, 0.0f };
	float cmpAttack = 1.0f;
	float cmpRelease = 1.0f;
	float refLin = 1.0f;
	float rangeKeyDb = -1.0f, rangeLo = 1.0f, rangeHi = 1.0f;
	float makeupKeyDb = 0.0f, makeupLin = 1.0f;

	/// **THE SECOND HALF (backlog QG: Compander and Excomper)**: its own follower per channel, its own detector state (the
	/// mean squares and the SC HPF it hears the LANE'S OUTPUT through), and its attack and release steps — the first
	/// half's while linked, `cmpAttack2`/`cmpRelease2`'s while not. Its Side is never the sidechain: it decodes what the
	/// lane hands back.
	std::array<float, 2> env2 { 0.0f, 0.0f };
	float meanSquare2A = 0.0f;
	float meanSquare2B = 0.0f;
	dyn::Biquad hp2L {};
	dyn::Biquad hp2R {};
	float cmpAttack2 = 1.0f;
	float cmpRelease2 = 1.0f;
	/// THE TEST SEAMS' NUMBERS: each half's last gain in dB per channel (signed), the second 0 when there is none.
	std::array<float, 2> firstDb { 0.0f, 0.0f };
	std::array<float, 2> secondDb { 0.0f, 0.0f };

	/// **THE LANE, AS THE HOLDER HANDS IT EACH BLOCK** (`comp_ops.cpp`; the Delay's `LoopInsert`). Null `process` is no
	/// lane (unbuilt, or holding nothing) and the halves meet directly. `latency` is the lane's, this host block.
	struct LaneInsert {
		void* context = nullptr;
		void (*process)(void* context, float* left, float* right, int frames) = nullptr;
		uint32_t latency = 0;
	};
	LaneInsert lane {};
	/// **THE LANE'S LATENCY, MATCHED EVERYWHERE ELSE** (backlog QG): the dry the Compander mixes against waits for the
	/// lane (`dryPad`); in Compress and Limit the whole output waits by it (`outPad`), so a Mode flip never moves the
	/// device's latency. Sized by the holder's `prepare` while a lane is built; unsized they delay nothing.
	device::CompensationDelay dryPad;
	device::CompensationDelay outPad;
	/// THE COMPANDER'S CHUNK: the two passes meet in these, so a host block is cut into pieces this long.
	static constexpr int kChunk = 256;
	std::array<float, kChunk> wetL {}, wetR {}, dryL {}, dryR {}, kAt {}, ratioAt {}, rangeAt {};

	/// EVERY CONTROL THAT SCALES AUDIO MOVES OVER `kRampSeconds` (the design: "nothing steps"). Ceiling
	/// is ramped too — it is a gain in Limit, and a stepped clamp is a click.
	///
	/// **AND STEREO LINK**, which moves no audio itself but steps every gain when the attack is instant (and
	/// in Limit it always is): a link stepped by a hand or a route would be a click.
	dyn::LinearRamp makeupDb, mix, driveDb, ceilingDb, link, bypass;
	/// THE COMPANDER'S RATIO AND RANGE, ramped too: both are gains, and both are modulatable.
	dyn::LinearRamp cmpRatio, cmpRange;
	/// THE SECOND HALF'S RATIO WHILE UNLINKED.
	dyn::LinearRamp cmpRatio2;
	/// `grA` and `grB` are each channel's own reduction (backlog LQ); `gr` is the deeper of the two.
	dyn::HeldPeak inPeak, outPeak, gr, grA, grB;

	device::ReadingsSink sink {};

	/// **THE PLUGIN'S SIDECHAIN FOR THE NEXT `process`** (backlog JO; `AudioOps::setSidechain`): valid for that one call only, and
	/// null is silence. Read by the detector alone while Detector is Side; the audio path never touches it. `scSounding` is
	/// `device::SidechainPresence`'s verdict, published for the card's "sidechain silent".
	const float* scL = nullptr;
	const float* scR = nullptr;
	int scFrames = 0;
	bool scSounding = false;

	/// **THE DELAY EVERY SAMPLE TAKES THROUGH THE DEVICE** (backlog LE): `latencySamplesFor` long, one
	/// record per sample, read when the sample leaves. A Compress sample enters finished (`Pass`, its
	/// output in `a`/`b`), so Compress is its zero-latency self delayed to the bit; a Limit sample
	/// enters as its DRY (`a`/`b`), with the ceiling and the enable it entered with, and leaves through
	/// its lookahead stage. The record is what makes every switch happen on the input's timeline.
	enum class Kind : uint8_t { Pass, Limit, LimitMidSide };
	struct Late {
		float a = 0.0f;
		float b = 0.0f;
		float ceiling = 1.0f;
		float enable = 0.0f;
		Kind kind = Kind::Pass;
	};
	std::vector<Late> late;
	size_t pos = 0;
	/// THE LOOKAHEAD STAGES (`dsp/lookahead_limiter.h`, on COMP's own detector through `processSidechain`;
	/// the L/R stage after M/S on its own input), each at a ceiling of 1 because what they hear is already
	/// over the ceiling. `lrLim` is L/R Limit's, the whole latency; `msLim` and `stageLim` share it in
	/// series in M/S (`midSideLookaheadFor`). A stage runs only while something it holds will be heard.
	dsp::LookaheadLimiter lrLim, msLim, stageLim;
	uint32_t lrBusy = 0, msBusy = 0, stageBusy = 0;

	/// THE STAGES' SETTINGS: COMP's Release, released IN dB as the zero-latency Limit released (the
	/// smoother's one-pole on the reduction in dB), so the control means what it meant. Its "off" is an
	/// instant release, which the block reads as a release of 1 — a time it cannot round to anything else.
	[[nodiscard]] dsp::LookaheadLimiter::Params stageParams(size_t lookahead, bool linked) const {
		const double r = timeSeconds(state.release, kReleaseMinSeconds);
		return { .ceiling = 1.0f,
			     .lookahead = lookahead,
			     .releaseSeconds = r > 0.0 ? r : 1e-12,
			     .link = linked,
			     .releaseInDecibels = true };
	}
	void aimStages() {
		const uint32_t n = latencySamplesFor(sampleRate);
		const uint32_t half = midSideLookaheadFor(n);
		lrLim.setParams(stageParams(n, false));
		msLim.setParams(stageParams(half, false));
		stageLim.setParams(stageParams(n - half, true));
	}

	/// THE POLES AND THE FILTER ARE FUNCTIONS OF SETTINGS AND RATE, so both roads recompute them.
	void refresh() {
		pAttack = poleFor(timeSeconds(state.attack, kAttackMinSeconds), sampleRate);
		pRelease = poleFor(timeSeconds(state.release, kReleaseMinSeconds), sampleRate);
		pRms = poleFor(kRmsSeconds, sampleRate);
		const double att = timeSeconds(state.attack, kAttackMinSeconds);
		const double rel = timeSeconds(state.release, kReleaseMinSeconds);
		cmpAttack = att > 0.0 ? dsp::compander::coefFor(att, sampleRate) : 1.0f;
		cmpRelease = rel > 0.0 ? dsp::compander::coefFor(rel, sampleRate) : 1.0f;
		if (state.cmpLink) {
			cmpAttack2 = cmpAttack;
			cmpRelease2 = cmpRelease;
		}
		else {
			const double att2 = timeSeconds(state.cmpAttack2, kAttackMinSeconds);
			const double rel2 = timeSeconds(state.cmpRelease2, kReleaseMinSeconds);
			cmpAttack2 = att2 > 0.0 ? dsp::compander::coefFor(att2, sampleRate) : 1.0f;
			cmpRelease2 = rel2 > 0.0 ? dsp::compander::coefFor(rel2, sampleRate) : 1.0f;
		}
		refLin = dbToLin(state.thresholdDb);
		const float hz = scHpfHz(state.scHpf);
		if (hz <= 0.0f) {
			// CLEARED WHEN BYPASSED, so switching it back on does not replay what it held.
			hpL.clear();
			hpR.clear();
			hp2L.clear();
			hp2R.clear();
		}
		else if (hz != hpHz || sampleRate != hpRate) {
			hpL.designHighpass(hz, sampleRate);
			hpR.designHighpass(hz, sampleRate);
			hp2L.designHighpass(hz, sampleRate);
			hp2R.designHighpass(hz, sampleRate);
		}
		hpHz = hz;
		hpRate = sampleRate;
		aimStages();
	}

	/// THE RAMPS ARE AIMED AT THE START OF EACH BLOCK, not in `setFields`, so the first block after a
	/// construct or a `reset` SNAPS to its settings (a `LinearRamp`'s first `set` snaps) whether or not
	/// `setFields` ran in between.
	void aimRamps() {
		makeupDb.set(state.makeupDb);
		mix.set(state.mix);
		driveDb.set(state.driveDb);
		ceilingDb.set(state.ceilingDb);
		link.set(state.stereoLink);
		cmpRatio.set(state.cmpRatio);
		cmpRange.set(state.cmpRangeDb);
		cmpRatio2.set(state.cmpRatio2);
		bypass.set(state.enabled ? 1.0f : 0.0f);
	}

	/// THE DETECTOR AND THE SMOOTHER FORGET EVERYTHING: "as if just switched on".
	void clearDetector() {
		meanSquareA = meanSquareB = 0.0f;
		gainDb = { 0.0f, 0.0f };
		env = { 0.0f, 0.0f };
		env2 = { 0.0f, 0.0f };
		meanSquare2A = meanSquare2B = 0.0f;
		firstDb = secondDb = { 0.0f, 0.0f };
		hp2L.clear();
		hp2R.clear();
		stageDb = 0.0f;
		detectorDb = -180.0f;
		hpL.clear();
		hpR.clear();
	}

	/// **THE STEREO LINK (backlog LQ): EACH CHANNEL'S DETECTOR LEVEL, IN dB, BLENDED TOWARD THE SHARED ONE.**
	/// `out[ch] = (1 - k) * own + k * shared`, where `shared` is the louder channel's level — computed exactly
	/// as the one detector before LQ computed it (`max` of the two linear levels, then one `log10`), and
	/// returned, because it is still what `detectorDbForTest` reports.
	///
	/// IN dB, BY CHOICE: a blend of two levels in dB is a blend of how far over the threshold each is, which
	/// is what the gain computer reads; a linear blend would let the louder channel dominate any Link above a
	/// few percent. **THE TWO-ENDED FORM IS EXACT AT BOTH ENDS** (Mix's reasoning): at k == 1 it is
	/// `0 * own + shared`, the shared level to the bit — which is what makes Link 100% the pre-LQ device
	/// rather than a close neighbour of it — and at k == 0 it is the channel's own.
	[[nodiscard]] static float linkedLevelsDb(float lvlA, float lvlB, float k, std::array<float, 2>& out) {
		const float shared = 20.0f * std::log10(std::max(finiteOr0(std::max(lvlA, lvlB)), kFloor));
		if (k >= 1.0f) {
			// THE COMMON CASE AND THE DEFAULT: two `log10`s a sample saved, and the same number either way.
			out = { shared, shared };
			return shared;
		}
		const float ownA = 20.0f * std::log10(std::max(finiteOr0(lvlA), kFloor));
		const float ownB = 20.0f * std::log10(std::max(finiteOr0(lvlB), kFloor));
		out = { (1.0f - k) * ownA + k * shared, (1.0f - k) * ownB + k * shared };
		return shared;
	}

	void publish() {
		if (sink.publish != nullptr) {
			// `Reading`'s declared order — the enum is the contract with the card.
			const float values[static_cast<size_t>(Reading::kCount)] = { inPeak.reading(), outPeak.reading(),
				                                                         gr.reading(), grA.reading(), grB.reading(),
			                                                         scSounding ? 1.0f : 0.0f };
			sink.publish(sink.context, values, static_cast<size_t>(Reading::kCount));
		}
	}

	/// ONE LOOKAHEAD STAGE'S TURN THIS SAMPLE. Fed `real` while its input is a sample that will be heard,
	/// and fed silence for as long as a real one is still inside it (`busy`), so the ones in flight leave
	/// on their own gains; idle otherwise, which costs nothing. **A STAGE COMING BACK FROM IDLE IS RESET**:
	/// nothing in it will be heard, and a fresh one carries no release from an episode that ended.
	/// Returns the stage's output (the input `lookahead` samples ago), or zeros while idle.
	static std::array<float, 2> step(dsp::LookaheadLimiter& lim, uint32_t& busy, bool real, float a, float b,
	                                 float peakA, float peakB, bool sidechain) {
		if (!real && busy == 0) {
			return { 0.0f, 0.0f };
		}
		if (real && busy == 0) {
			lim.reset();
		}
		if (!real) {
			a = b = peakA = peakB = 0.0f;
		}
		if (sidechain) {
			lim.processSidechain(&a, &b, &peakA, &peakB, 1);
		}
		else {
			lim.process(&a, &b, 1);
		}
		busy = real ? static_cast<uint32_t>(lim.lookahead()) : busy - 1;
		return { a, b };
	}

	void process(float* left, float* right, int frames);
	/// **THE COMPANDER'S ROAD** (backlog QG, `comp.cpp`): the first half into `wet`, the lane over it, the second half (if
	/// paired) on what it hands back, then makeup, mix against the padded dry, the enable — and out through `emit`.
	void processCompander(float* left, float* right, int frames);
	/// **ONE SAMPLE OUT** — the record that entered `late.size()` samples ago, by the road it entered on, the Limit stages
	/// stepped for whatever is in flight. Every mode's loop ends here, so a flip between them drops and replays nothing.
	std::array<float, 2> emit(const Late& in, float aA, float aB, float pA, float pB);
};

} // namespace feathers::fx::comp
