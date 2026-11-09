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

#include "fx/resonate/resonate_dsp.h"


#include "dsp/portable_trig.h"
#include "pffft/pffft.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <iterator>
#include <type_traits>
#include <utility>

namespace feathers::fx::resonate {

namespace {

/// THE B SIDE OF THE LAWS: Character plus a fixed golden-section offset, so Morph always heads somewhere
/// else on the same map — the synth engine's A/B pairing, with the second end derived rather than stored.
constexpr double kMorphOffset = 0.3819660112501051;
/// Character must move this far before the laws are rebuilt (a rebuild is two law builds and a solve).
constexpr float kCharacterEpsilon = 1.0e-4f;
constexpr float kTwoPi = 6.283185307f;
/// The kernel is normalised to 1 / (peak x this): see buildKernel.
constexpr float kPeakMargin = 1.001f;
/// Morph moving more than this between two ticks counts as moving the string.
constexpr float kMorphMotionEpsilon = 1.0e-5f;
/// A glide this close to its key lands on it, rather than approaching for ever (a millionth of a cent).
constexpr double kGlideLandSemis = 1.0e-8;
/// CLAP's per-note TUNING expression (CLAP_NOTE_EXPRESSION_TUNING), in semitones; a bend this close to its target lands on it.
constexpr int32_t kTuningExpression = 2;
constexpr double kBendLandSemis = 1.0e-8;
/// CLAP's range for the tuning expression.
constexpr double kMaxBendSemis = 120.0;

static_assert(static_cast<int>(Model::kCount) == kWeaveModelCount, "Resonate's Model menu is WeaveModel's");
static_assert(static_cast<int>(Model::Stiffening) == static_cast<int>(WeaveModel::Stiffening));
static_assert(static_cast<int>(Model::Membrane) == static_cast<int>(WeaveModel::Membrane));

float smoothstep(float t) {
	return t * t * (3.0f - 2.0f * t);
}

/// A 0..1 control as the DSP reads it: clamped, and a non-finite value as 0.
float clamp01(float v) {
	return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

double wrap01(double p) {
	return p - std::floor(p);
}

} // namespace

ResonateDsp::~ResonateDsp() {
	if (fft_ != nullptr) {
		pffft_destroy_setup(fft_);
	}
}

void ResonateDsp::prepare(double sampleRate) {
	sr_ = std::max(sampleRate, 1.0);
	if (fft_ == nullptr) {
		fft_ = pffft_new_setup(kPeakFftSize, PFFFT_REAL);
	}
	// EVERY STRING'S EAR LINE, for the lowest pitch an ear tunes to at this rate — here, never on the audio thread.
	keyVelocity_.fill(1.0f);
	keyTuning_.fill(0.0f);
	keyBurst_.fill(0.0f);
	earLength_ = HarmonicEar::lineLength(sr_);
	earLines_.assign(kMaxDroneStrings * static_cast<size_t>(earLength_), 0.0f);
	for (size_t k = 0; k < kMaxDroneStrings; ++k) {
		resetString(k);
	}
	follower_.prepare(sr_);
	follower_.setTimes(kFollowAttack, kFollowRelease);
	for (dyn::LinearRamp* r : ramps_) {
		r->prepare(sr_);
	}
	restartPending_ = true;
	queued_ = 0;
}

/// THE KERNEL'S LARGEST GAIN AT ANY FREQUENCY, from a 2048-point real FFT of it zero-padded — eight points
/// per cycle of the longest kernel's (256 taps') finest ripple, so the grid peak sits within a small,
/// measured fraction of the true one — refined by a parabola. Twice a tick; no allocation.
float ResonateDsp::peakMagnitude(const float* kernel, int taps) {
	constexpr int kN = kPeakFftSize;
	std::copy(kernel, kernel + taps, fftIn_.begin());
	std::fill(fftIn_.begin() + taps, fftIn_.end(), 0.0f);
	pffft_transform_ordered(fft_, fftIn_.data(), fftOut_.data(), fftWork_.data(), PFFFT_FORWARD);
	// ORDERED REAL OUTPUT: [0] is DC and [1] Nyquist, both real; then (re, im) for bins 1 .. N/2 - 1.
	const auto mag2 = [&](int k) {
		if (k == 0) {
			return fftOut_[0] * fftOut_[0];
		}
		if (k == kN / 2) {
			return fftOut_[1] * fftOut_[1];
		}
		const float re = fftOut_[static_cast<size_t>(2 * k)];
		const float im = fftOut_[static_cast<size_t>(2 * k + 1)];
		return re * re + im * im;
	};
	int top = 0;
	float topMag2 = 0.0f;
	for (int k = 0; k <= kN / 2; ++k) {
		const float m2 = mag2(k);
		if (m2 > topMag2) {
			topMag2 = m2;
			top = k;
		}
	}
	// THE PEAK BETWEEN GRID POINTS: a parabola through the top bin and its neighbours. The grid alone read
	// up to 0.23% low (MEASURED against a grid eight times finer), which is a boost the normalisation missed.
	const float peak = std::sqrt(topMag2);
	if (top == 0 || top == kN / 2) {
		return peak;
	}
	const float a = std::sqrt(mag2(top - 1));
	const float c = std::sqrt(mag2(top + 1));
	const float curve = a - 2.0f * peak + c;
	return (curve < 0.0f) ? std::max(peak, peak - 0.125f * (a - c) * (a - c) / curve) : peak;
}

void ResonateDsp::reset() {
	restartPending_ = true;
	queued_ = 0;
}

void ResonateDsp::queueNotes(const device::NoteEvent* in, size_t count) {
	for (size_t i = 0; i < count && queued_ < kMaxQueuedNotes; ++i) {
		queue_[queued_++] = in[i];
	}
}

void ResonateDsp::rebuildLaws() {
	lawsA_ = owlet::dsp::buildPhiWeaveLaws(static_cast<double>(settings_.character));
	lawsB_ = owlet::dsp::buildPhiWeaveLaws(static_cast<double>(settings_.character) + kMorphOffset);
	// RING DECAY: the synth's Explore Decay law on the ring's damping — damping / 2^value, clamped, the bow balance
	// re-derived by the builder's own energy-balance law — with ONE DIFFERENCE, the floor: the builder's 0.006 is where
	// most characters' damping already sits, so Up had no room (MEASURED at Character 0.5: x4 lengthened the ring by
	// x1.06). Here the floor is kRingDampMin, a quarter of it. The loop stays damped (d > 0) and the integrator's edge,
	// lambda < 16 - 4d (weave_models.h), only relaxes as d falls. x1 applies nothing, bit for bit.
	const float decay = std::isfinite(settings_.ringDecay) ? std::clamp(settings_.ringDecay, -2.0f, 2.0f) : 0.0f;
	if (decay != 0.0f) {
		const float scale = std::exp2(decay);
		for (owlet::dsp::PhiWeaveParams* laws : { &lawsA_, &lawsB_ }) {
			for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
				laws->damping[i] = std::clamp(laws->damping[i] / scale, kRingDampMin, kRingDampMax);
				laws->bowBalance[i] = 0.60f * std::min(1.0f, std::sqrt(laws->damping[i] * (1.0f / 0.0255f)));
			}
		}
	}
	// TENSION: the ring's motion t times as fast — its springs and couplings by t^2 (w ~ sqrt(k)), so every mode moves by
	// t and the rest pose, a balance of the two, does not; and the bow and the morph agitation by t^2 too, so what a force
	// holds the string at (force / k) stays where it was instead of growing 256-fold at a sixteenth. Damping is per
	// sub-step and untouched, so the ring decays in the same time. BOUNDED at the top: x2 is k and c x4, and the largest
	// k + 4c of any character is 1.92, so 7.7 against the integrator's edge 16 - 4d (weave_models.h; test_fx_resonate
	// sweeps every model at x2). x1 applies nothing, bit for bit. EXCEPT STIFFENING'S TOP: its cubic spring is 8c too, so
	// it grows with the tension and with the stretch, and driven hard at x2 its row came within 1.1 of that edge (0.66 at
	// x1.6; 9.8 at x1, MEASURED 2026-09-28) — so on Stiffening the tension stops at x1, and only slows it.
	const float top = settings_.model == static_cast<int>(Model::Stiffening) ? 0.0f : 1.0f;
	const float tension = std::isfinite(settings_.tension) ? std::clamp(settings_.tension, -4.0f, top) : 0.0f;
	if (tension != 0.0f) {
		const float t2 = std::exp2(2.0f * tension);
		for (owlet::dsp::PhiWeaveParams* laws : { &lawsA_, &lawsB_ }) {
			for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
				laws->stiffness[i] *= t2;
				laws->coupling[i] *= t2;
			}
			laws->bowDepth *= t2;
			laws->morphBowGain *= t2;
		}
	}
	ring_.setLaws(lawsA_, lawsB_);
	lawsCharacter_ = settings_.character;
	lawsDecay_ = settings_.ringDecay;
	lawsTension_ = settings_.tension;
}

void ResonateDsp::applySettings(bool snap) {
	const Settings& s = settings_;
	// THE LEVEL FADER'S LAW: gain = (2 x level)^2, so its default kFaderUnity (0.5) is unity, 1 is +12 dB and the travel
	// below the middle is squared toward silence, the way a fader's taper spends its throw.
	const float levelGain = (2.0f * s.level) * (2.0f * s.level);
	const float targets[] = { s.morph, s.blend, levelGain, s.mix, s.width, s.enabled ? 1.0f : 0.0f, s.sustain, (2.0f * s.droneOut) * (2.0f * s.droneOut),
		                        std::clamp(std::isfinite(s.sympathy) ? s.sympathy : 0.0f, 0.0f, 1.0f),
		                        std::clamp(std::isfinite(s.droneSympathy) ? s.droneSympathy : 0.0f, 0.0f, 1.0f) };
	static_assert(std::size(targets) == std::extent_v<decltype(ramps_)>, "one target per ramp");
	for (size_t i = 0; i < std::size(ramps_); ++i) {
		if (snap) {
			ramps_[i]->snap(targets[i]);
		}
		else {
			ramps_[i]->set(targets[i]);
		}
	}
	if (snap || std::abs(s.character - lawsCharacter_) > kCharacterEpsilon || s.ringDecay != lawsDecay_ || s.tension != lawsTension_
	    || (s.model != lawsModel_ && s.tension > 0.0f)) {
		rebuildLaws();
	}
	if (snap || s.model != lawsModel_) {
		ring_.setModel(static_cast<WeaveModel>(std::clamp(s.model, 0, kWeaveModelCount - 1)));
		lawsModel_ = s.model;
	}
	// THE JAWARI LINE: set every time (a copy and an offset under each node), read at the next sub-tick. Unramped: a line that jumps
	// can only put a node back toward rest, never kick it (WeaveString::setBridge).
	ring_.setBridge({ s.jawari, s.bridgeCurve, clamp01(s.bounce) });
	if (s.pickup != pickupApplied_) {
		pickupApplied_ = s.pickup;
		for (NodeString& v : voiceStrings_) {
			v.setPickup(s.pickup);
		}
		for (DroneString& d : strings_) {
			d.node.setPickup(s.pickup);
		}
	}
}

void ResonateDsp::restart() {
	restartPending_ = false;
	applySettings(true);
	droneInputGain_ = (2.0f * std::clamp(std::isfinite(settings_.droneInput) ? settings_.droneInput : 0.5f, 0.0f, 1.0f))
	                  * (2.0f * std::clamp(std::isfinite(settings_.droneInput) ? settings_.droneInput : 0.5f, 0.0f, 1.0f));
	ring_.setCrossfade(morph_.value());
	ring_.setExcitation({ kBowGain * settings_.bow });
	for (NodeString& v : voiceStrings_) {
		v.prepare(sr_);
	}
	stringBlend_ = std::clamp(std::isfinite(settings_.stringBlend) ? settings_.stringBlend : 0.0f, 0.0f, 1.0f);
	stringExcite_ = std::clamp(std::isfinite(settings_.stringExcite) ? settings_.stringExcite : 0.0f, 0.0f, 1.0f);
	proxPrev_ = proxTarget_ = proxRef_ = proximity_ = 0.0f;
	ring_.setRestTables(true); // the heads' strings are driven by the ring's MOTION (v15): its scan less its rest shape
	ring_.setMotionSmoothing(kMotionSmoothing);
	ring_.prepare(sr_);
	// A FRESH STRING IS STILL: no pluck of its own (the core's reset queues one). A note-on plucks it.
	ring_.pluck(0.0f);

	follower_.reset();
	meterLevel_ = 0.0f;
	pluckCount_ = 0;
	lastPluckPos_ = 0.0f;
	pluckAge_ = kPluckAgeMax;

	heads_ = {};
	for (size_t k = 0; k < kMaxDroneStrings; ++k) {
		resetString(k);
	}
	bridgeW_ = bridgeWPrev_ = bridgeMass_ = 0.0;
	bridgeSympathyAt_ = bridgeDroneSympathyAt_ = -1.0f; // recomputed at the next sample
	bridgeYields_ = false;
	cycleCountdown_ = 0;
	cycleStep_ = 0;
	clearBody();

	monoSlot_ = -1;
	lastVoicing_ = settings_.voicing;
	lastTickMorph_ = parkedMorph_ = morph_.value();
	onsetCounter_ = 0;

	gate_ = gatePrev_ = gateTarget_ = gateEnergy_ = 0.0f;
	gateLatched_ = excitedThisWindow_ = playedThisWindow_ = played_ = false;
	seatKernels();
	for (auto& h : history_) {
		h.fill(0.0f);
	}
	historyPos_ = 0;
	idleSamples_ = 0;
	resting_ = false;
	restPublishCount_ = 0;
}

double ResonateDsp::bridgeEnergyForTest() const {
	double e = 0.0;
	for (const DroneString& d : strings_) {
		e += d.node.yielding() ? d.node.energyOffBridge() : d.node.energy();
	}
	for (const NodeString& v : voiceStrings_) {
		e += v.yielding() ? v.energyOffBridge() : v.energy();
	}
	if (bridgeMass_ > 0.0) {
		const double spring = (2.0 * 3.14159265358979 * kBridgeHz / sr_) * (2.0 * 3.14159265358979 * kBridgeHz / sr_);
		const double dw = bridgeW_ - bridgeWPrev_;
		e += bridgeMass_ * (dw * dw + spring * bridgeW_ * bridgeWPrev_);
	}
	return e;
}

void ResonateDsp::strikeDroneForTest(size_t k, float velocity) {
	strings_[k].node.strike(kCyclePluckAmp * velocity * cycleGain(), std::max(1, static_cast<int>(std::lround(kDroneStrikeSeconds * sr_))));
	resting_ = false;
	idleSamples_ = 0;
}

void ResonateDsp::resetString(size_t k) {
	strings_[k] = DroneString {};
	strings_[k].node.setReferenceForTest(referenceForTest_); // a fresh string keeps the case's road
	if (earLength_ > 0) {
		strings_[k].ear.attach(earLines_.data() + k * static_cast<size_t>(earLength_), earLength_);
		strings_[k].node.prepare(sr_);
		strings_[k].node.setPickup(pickupApplied_);
	}
}

double ResonateDsp::semitonesOffset() const {
	return 12.0 * settings_.octave + settings_.semitone + static_cast<double>(settings_.fine) / 100.0;
}

double ResonateDsp::hzOf(double semis) const {
	return 440.0 * std::exp2((semis - 69.0) / 12.0);
}

// ============================================================================
// NOTES. What the host holds is kept as the HELD KEYS, in the order struck, whatever the Voicing; heads
// are how those keys sound. So a Voicing change can release every head and play the same keys again in
// the new mode, and Mono's last-note priority reads the same list Poly allocates from.
// ============================================================================

void ResonateDsp::notePluck(int key, float velocityGain) {
	// A NOTE-ON PLUCKS THE RING (v19, by design; it replaces the input's onset detector): once, velocity^2 x Pluck, at the
	// laws' pluck position (the morph between the two models'), and its scan burst waits for its own head (startHead).
	const float amp = kPluckMax * velocityGain * pluckGain(settings_.pluck);
	keyBurst_[static_cast<size_t>(std::clamp(key, 0, 127))] = amp;
	if (amp <= 0.0f) {
		return;
	}
	const float cf = morph_.value();
	lastPluckPos_ = (1.0f - cf) * lawsA_.pluckPos + cf * lawsB_.pluckPos;
	ring_.pluck(amp, lastPluckPos_);
	lastRingPluckAmp_ = amp;
	++pluckCount_;
	pluckAge_ = 0.0f;
	excitedThisWindow_ = playedThisWindow_ = true;
}

void ResonateDsp::startHead(Head& h, int key) {
	h.key = key;
	h.semis = key; // a fresh head has nothing to glide from
	h.bend = keyTuning_[static_cast<size_t>(std::clamp(key, 0, 127))]; // nor to bend from
	h.phase = 0.0;
	h.held = true;
	h.live = true;
	h.ringing = false;
	h.released = false;
	h.driveEnv = 0.0f;
	h.stealing = false;
	h.onset = ++onsetCounter_;
	// ITS NOTE-ON'S SCAN BURST, its own (notePluck), taken once.
	h.drivePluck = std::exchange(keyBurst_[static_cast<size_t>(std::clamp(key, 0, 127))], 0.0f);
	h.inc = hzOf(h.semis + h.bend + semitonesOffset()) / sr_;
	// A FRESH NOTE ON A FRESH STRING: whatever the head played before is gone.
	voiceStrings_[static_cast<size_t>(&h - heads_.data())].clear();
}

ResonateDsp::Head* ResonateDsp::allocate(int key) {
	for (Head& h : heads_) {
		if (!h.live) {
			startHead(h, key);
			return &h;
		}
	}
	// EVERY HEAD IS SOUNDING OR FADING: one is stolen, through a fast fade so its phase never jumps at gain.
	// A head already RELEASING goes first — the oldest of those — and only then the oldest one a key holds (a
	// head mid-steal counts as held while the key it will start is; stolen again, it just changes that key).
	Head* victim = nullptr;
	bool victimHeld = true;
	for (Head& h : heads_) {
		const bool held = h.stealing ? h.nextHeld : h.held;
		if (victim == nullptr || (!held && victimHeld) || (held == victimHeld && h.onset < victim->onset)) {
			victim = &h;
			victimHeld = held;
		}
	}
	victim->stealing = true;
	victim->held = false;
	victim->ringing = false;
	victim->nextKey = key;
	victim->nextHeld = true;
	victim->onset = ++onsetCounter_;
	return victim;
}

// A LET-GO HEAD SOUNDS ON ITS OWN ENVELOPE (released notes that had stopped ringing came back when new
// notes played; v14, the audit: in Ring a released head still burst +8-13 dB on a new onset and sat +3.5 dB above a
// control, its cap recomputed only at ring ticks and its fall ramp-limited). The motion gate is the WHOLE ring's, so a head
// read through it is re-opened by any later onset. So from the note-off its scanned voice's gain is held under its gain
// then, decaying at the ring's own rate (the strings' T60, Ring Decay's): a PEAK-HOLD, which a new onset cannot lift and
// which, unlike a running minimum, does not lock onto a trough of the bow's noise (the audit measured -3.7 dB). Its node
// string is no longer driven, and rings on its own loss (the process loop). In Ring it sounds until the ring has come to
// rest or its hold has fallen kRingCloseLevel under its start, then ramps out; in Gate it ramps out at once.
void ResonateDsp::startRelease(Head& h) {
	h.released = true;
	h.hold = h.gain * lastToneGain_;
	h.holdRef = std::max(h.hold, 1e-12f);
	h.scanCap = h.hold;
	h.stringGain = lastToneGain_;
	h.ringing = ringRelease();
}

bool ResonateDsp::keyHeld(int key) const {
	for (int i = 0; i < heldCount_; ++i) {
		if (held_[i].key == key) {
			return true;
		}
	}
	return false;
}

void ResonateDsp::voiceOn(int key) {
	if (settings_.voicing == static_cast<int>(Voicing::Mono)) {
		if (monoSlot_ >= 0 && heads_[static_cast<size_t>(monoSlot_)].live) {
			Head& h = heads_[static_cast<size_t>(monoSlot_)];
			if (h.stealing) {
				h.nextKey = key;
				h.nextHeld = true;
			}
			else {
				// LEGATO (or re-taking a voice still fading): the glide carries its pitch on toward the new key.
				h.released = false;
				h.ringing = false;
				h.held = true;
				h.key = key;
			}
			return;
		}
		monoSlot_ = static_cast<int>(allocate(key) - heads_.data());
		return;
	}
	for (Head& h : heads_) {
		if ((h.held && h.key == key) || (h.stealing && h.nextKey == key && h.nextHeld)) {
			return; // the same key again: the same head
		}
	}
	allocate(key);
}

void ResonateDsp::voiceOff(int key) {
	if (settings_.voicing == static_cast<int>(Voicing::Mono)) {
		if (monoSlot_ < 0) {
			return;
		}
		Head& h = heads_[static_cast<size_t>(monoSlot_)];
		if (heldCount_ > 0) {
			// LAST-NOTE PRIORITY: back to the newest key still down, and the head's key follows it.
			const int newest = held_[heldCount_ - 1].key;
			if (h.stealing) {
				h.nextKey = newest;
			}
			else {
				h.key = newest;
			}
		}
		else if (h.stealing) {
			h.nextHeld = false;
		}
		else {
			h.held = false;
			startRelease(h);
		}
		return;
	}
	// THE SAME KEY STILL HELD on another channel or under another id: its head plays on.
	if (keyHeld(key)) {
		return;
	}
	for (Head& h : heads_) {
		if (h.held && h.key == key) {
			h.held = false;
			startRelease(h);
		}
		if (h.stealing && h.nextKey == key) {
			h.nextHeld = false;
		}
	}
}

void ResonateDsp::physicalOn(const device::NoteEvent& e) {
	// THE SAME NOTE AGAIN (same key, channel and id) moves to the top rather than holding twice.
	int w = 0;
	for (int i = 0; i < heldCount_; ++i) {
		const HeldKey& k = held_[i];
		if (!(k.key == e.key && k.channel == e.channel && k.noteId == e.noteId)) {
			held_[w++] = k;
		}
	}
	heldCount_ = w;
	// FULL: the oldest is dropped, and released only after the new key is on the list, so a drop of the key
	// being struck again (on another channel, under another id) leaves its head playing.
	int dropped = -1;
	if (heldCount_ == kMaxHeldKeys) {
		dropped = held_[0].key;
		std::copy(held_ + 1, held_ + kMaxHeldKeys, held_);
		--heldCount_;
	}
	held_[heldCount_++] = HeldKey { e.key, e.channel, e.noteId };
	if (dropped >= 0) {
		voiceOff(dropped);
	}
	// ITS VELOCITY (v13): how hard its string is played; and (v19) its ring pluck and its own head's scan burst.
	const float vg = velocityGain(e.velocity);
	const size_t k = static_cast<size_t>(std::clamp(static_cast<int>(e.key), 0, 127));
	keyVelocity_[k] = vg;
	notePluck(e.key, vg);
	voiceOn(e.key);
	// A HEAD THAT DID NOT START (Mono's legato glides the one already sounding) takes its burst now; a stolen slot still
	// fading takes it when it starts.
	if (keyBurst_[k] > 0.0f) {
		for (Head& h : heads_) {
			if (h.live && h.held && !h.stealing && h.key == e.key) {
				h.drivePluck = std::exchange(keyBurst_[k], 0.0f);
				break;
			}
		}
	}
}

void ResonateDsp::physicalOff(const device::NoteEvent& e, bool choke) {
	// CLAP'S ADDRESSING: -1 on either side of any of id, channel and key is a wildcard — the way
	// dahdsr.cpp and noteexpr.cpp match — so an all-notes-off (all -1) releases everything.
	int w = 0;
	int released[kMaxHeldKeys];
	int nReleased = 0;
	for (int i = 0; i < heldCount_; ++i) {
		const HeldKey& k = held_[i];
		const bool idMatches = e.noteId == -1 || k.noteId == -1 || k.noteId == e.noteId;
		const bool matches = idMatches && (e.channel == -1 || k.channel == e.channel) && (e.key == -1 || k.key == e.key);
		if (matches) {
			released[nReleased++] = k.key;
		}
		else {
			held_[w++] = k;
		}
	}
	heldCount_ = w;
	for (int i = 0; i < nReleased; ++i) {
		voiceOff(released[i]);
		if (choke && !keyHeld(released[i])) {
			chokeKey(released[i]);
		}
	}
	if (choke && monoSlot_ >= 0 && !heads_[static_cast<size_t>(monoSlot_)].live) {
		monoSlot_ = -1;
	}
}

void ResonateDsp::chokeKey(int key) {
	for (Head& h : heads_) {
		if (!h.live) {
			continue;
		}
		if (h.stealing) {
			// A STEAL IN PROGRESS: what is fading out is the choked note, but the key it will start is
			// another, still held — so the steal finishes now, from silence, rather than dying with it. (A
			// choke of the key it would START needs nothing here: voiceOff cancelled that start, and the old
			// note's fade runs out as it would have.)
			if (h.key == key) {
				if (h.nextHeld) {
					startHead(h, h.nextKey);
					h.gain = 0.0f;
				}
				else {
					h.gain = 0.0f;
					h.live = false;
					h.stealing = false;
				}
			}
		}
		else if (!h.held && h.key == key) {
			h.gain = 0.0f;
			h.live = false;
			h.ringing = false;
		}
	}
}

void ResonateDsp::revoice() {
	for (Head& h : heads_) {
		h.held = false;
		h.ringing = false; // a Voicing change releases every head on the normal ramp, Ring or not
		h.stealing = false; // a pending start is cancelled; the head just finishes fading, at the pitch it had
	}
	monoSlot_ = -1;
	if (settings_.voicing == static_cast<int>(Voicing::Mono)) {
		if (heldCount_ > 0) {
			voiceOn(held_[heldCount_ - 1].key);
		}
		return;
	}
	for (int i = 0; i < heldCount_; ++i) {
		voiceOn(held_[i].key);
	}
}

void ResonateDsp::applyNotes() {
	for (int i = 0; i < queued_; ++i) {
		const device::NoteEvent& e = queue_[i];
		switch (e.kind) {
		case device::NoteEvent::Kind::On:
			if (e.velocity > 0.0 && e.key >= 0 && e.key < 128) {
				keyTuning_[static_cast<size_t>(e.key)] = 0.0f; // a fresh note is untuned
			}
			if (e.velocity > 0.0) {
				physicalOn(e);
			}
			else {
				physicalOff(e, false);
			}
			break;
		case device::NoteEvent::Kind::Off:
			physicalOff(e, false);
			break;
		case device::NoteEvent::Kind::Choke:
			physicalOff(e, true);
			break;
		case device::NoteEvent::Kind::Expression:
			// A KEY'S TUNING (CLAP_NOTE_EXPRESSION_TUNING, semitones, absolute): its heads bend toward it (glide); a negative
			// key is CLAP's wildcard and tunes every key. The drone's strings are not played by keys and never bend.
			if (e.expressionId == kTuningExpression && std::isfinite(e.expressionValue)) {
				const float semis = static_cast<float>(std::clamp(e.expressionValue, -kMaxBendSemis, kMaxBendSemis));
				if (e.key < 0) {
					keyTuning_.fill(semis);
				}
				else if (e.key < 128) {
					keyTuning_[static_cast<size_t>(e.key)] = semis;
				}
			}
			break;
		}
	}
	queued_ = 0;
}

bool ResonateDsp::anyNoteHeadLive() const {
	for (const Head& h : heads_) {
		if (h.live) {
			return true;
		}
	}
	return false;
}

int ResonateDsp::heldNotesForTest() const {
	return heldCount_; // keys physically held, whatever the Voicing
}

double ResonateDsp::headIncrementForTest(int slot) const {
	if (slot >= static_cast<int>(kMaxHeads)) {
		const DroneString& d = strings_[static_cast<size_t>(slot) - kMaxHeads];
		return d.gain > 0.0f ? d.inc : 0.0;
	}
	const Head& h = heads_[static_cast<size_t>(slot)];
	return h.live ? h.inc : 0.0;
}

float ResonateDsp::headGainForTest(int slot) const {
	if (slot >= static_cast<int>(kMaxHeads)) {
		return strings_[static_cast<size_t>(slot) - kMaxHeads].gain;
	}
	return heads_[static_cast<size_t>(slot)].gain;
}

// ============================================================================
// THE DRONE'S STRINGS
// ============================================================================

ResonateDsp::DroneResponse ResonateDsp::droneResponseFor(const Settings& s) {
	DroneResponse r;
	// THE CHORD IS A CHOICE and the PHASE a separate unbounded number : the phase drives every
	// string's triangles and never changes which chord sounds.
	r.zone = std::clamp(s.droneChord, 0, static_cast<int>(std::size(kDroneChords)) - 1);
	r.coordinate = std::isfinite(s.droneCharacter) ? std::clamp(static_cast<double>(s.droneCharacter), -kDroneCoordMax, kDroneCoordMax)
	                                           : kDroneCharacterDefault;
	r.position = static_cast<float>(r.coordinate - std::floor(r.coordinate));
	const DroneChord& chord = kDroneChords[static_cast<size_t>(r.zone)];
	r.count = chord.count;
	const float attackKnob = static_cast<float>(std::exp2(static_cast<double>(s.droneAttack)));
	const float decayKnob = static_cast<float>(std::exp2(static_cast<double>(s.droneDecay)));
	float power = 0.0f;
	for (int i = 0; i < r.count; ++i) {
		const size_t k = static_cast<size_t>(i);
		r.intervals[k] = chord.intervals[k];
		// ITS PITCH ABOVE ROOT IN THE TUNING: Equal is the semitone count exactly; Just its ratio, in (fractional) semitones.
		r.pitch[k] = s.droneTuning == static_cast<int>(Tuning::Just)
		                 ? 12.0 * std::log2(static_cast<double>(chord.just[k].num) / static_cast<double>(chord.just[k].den))
		                 : static_cast<double>(chord.intervals[k]);
		// EACH STRING'S OWN TRIANGLES: the coordinate is the phase, the string's interval places it.
		const double phase = r.coordinate
		                     + static_cast<double>(r.intervals[k]) * static_cast<double>(kPhiDroneStringSpread);
		const float tA = owlet::dsp::phi::evalTriangle(phase, 1.0f, kPhiDroneAttack);
		const float tD = owlet::dsp::phi::evalTriangle(phase, 1.0f, kPhiDroneDecay);
		const float tL = owlet::dsp::phi::evalTriangle(phase, 1.0f, kPhiDroneLevel);
		const float tQ = owlet::dsp::phi::evalTriangle(phase, 1.0f, kPhiDroneQ);
		r.attackScale[k] = kChordTimeTight * std::pow(kChordTimeBloom / kChordTimeTight, tA);
		r.decayScale[k] = kChordTimeTight * std::pow(kChordTimeBloom / kChordTimeTight, tD);
		r.q[k] = kChordQTight * std::pow(kChordQBloom / kChordQTight, tQ);
		// THE SAME TRIANGLE IS ITS VOICE'S BRIGHTNESS: a sharp ear, a bright string.
		r.brightness[k] = tQ;
		r.levelDb[k] = kChordLevelLowDb + (kChordLevelHighDb - kChordLevelLowDb) * tL;
		r.attackSeconds[k] = attackKnob * r.attackScale[k];
		r.t60Seconds[k] = decayKnob * r.decayScale[k];
		r.level[k] = std::pow(10.0f, r.levelDb[k] / 20.0f);
		power += r.level[k] * r.level[k];
	}
	// THE CHORD AT ONE STRING'S POWER, so it never gets louder by having more strings.
	const float norm = 1.0f / std::sqrt(power);
	for (int i = 0; i < r.count; ++i) {
		r.level[static_cast<size_t>(i)] *= norm;
	}
	// A TANPURA'S UNISON PAIR, AND ANY OTHER: strings on one interval spread symmetrically by the detune, so they
	// beat rather than sum. And every string's place between left and right, by its index.
	for (int i = 0; i < r.count; ++i) {
		const size_t k = static_cast<size_t>(i);
		int same = 0;
		int rank = 0;
		for (int j = 0; j < r.count; ++j) {
			if (r.intervals[static_cast<size_t>(j)] == r.intervals[k]) {
				rank += j < i ? 1 : 0;
				++same;
			}
		}
		r.detuneCents[k] = same > 1 ? static_cast<float>(kDroneUnisonDetuneCents
		                                                 * (2.0 * rank / static_cast<double>(same - 1) - 1.0))
		                            : 0.0f;
		r.pan[k] = owlet::dsp::phi::evalTriangle(static_cast<double>(i) * static_cast<double>(kPhiDroneStringSpread), 1.0f,
		                                          kPhiDronePan);
	}
	return r;
}

const ResonateDsp::DroneResponse& ResonateDsp::droneResponse() {
	const Settings& s = settings_;
	// COMPARED AS BITS, so a -0 or a NaN is never taken for what it is not (a NaN always makes it again).
	const auto same = [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b) && a == a; };
	if (referenceForTest_ || !droneRespValid_ || s.droneChord != droneRespChord_ || s.droneTuning != droneRespTuning_
	    || !same(s.droneCharacter, droneRespCharacter_) || !same(s.droneAttack, droneRespAttack_) || !same(s.droneDecay, droneRespDecay_)) {
		droneResp_ = droneResponseFor(s);
		droneRespValid_ = true;
		droneRespChord_ = s.droneChord;
		droneRespTuning_ = s.droneTuning;
		droneRespCharacter_ = s.droneCharacter;
		droneRespAttack_ = s.droneAttack;
		droneRespDecay_ = s.droneDecay;
	}
	return droneResp_;
}

void ResonateDsp::configureString(DroneString& d, double offset) {
	d.semis = static_cast<double>(d.root) + d.pitch + static_cast<double>(d.detune) / 100.0;
	const double hz = hzOf(d.semis + offset);
	d.inc = hz / sr_;
	const double st = std::clamp(std::isfinite(settings_.droneStiffness) ? static_cast<double>(settings_.droneStiffness) : 0.0, 0.0, 1.0);
	// NOTHING IT IS MADE FROM MOVED: the tunings below would be made again to the same bits (backlog PS).
	// DRONE DETAIL (backlog PS): the node cap its choice names, taken by the node string at the tune below (a regrid as it rings).
	const int cap = kDroneDetailNodes[static_cast<size_t>(std::clamp(settings_.droneDetail, 0, static_cast<int>(std::size(kDroneDetailNodes)) - 1))];
	d.node.setNodeCap(cap);
	if (!referenceForTest_ && d.tunedValid && cap == d.tunedCap && hz == d.tunedHz && sr_ == d.tunedSr && st == d.tunedStiffness && d.q == d.tunedQ
	    && d.t60 == d.tunedT60 && d.brightness == d.tunedBrightness) {
		return;
	}
	d.tunedValid = true;
	d.tunedCap = cap;
	d.tunedHz = hz;
	d.tunedSr = sr_;
	d.tunedStiffness = st;
	d.tunedQ = d.q;
	d.tunedT60 = d.t60;
	d.tunedBrightness = d.brightness;
	d.levelOwn = static_cast<float>(1.0 - std::exp(-d.inc / kScanLevelPeriods));
	// THE STRING'S EAR: a harmonic comb at its own pitch, as sharp as its Q (resonate_ear.h).
	d.ear.tune(hz, d.q, sr_);
	// ITS VOICE (v11): a node string at its pitch, ringing for its T60, as bright as its triangle (its frequency-dependent
	// loss x 4 dark .. x 1/4 bright, geometrically: where v10's comb set its loop low-pass from 3 to 24 harmonics), and as
	// stiff as the knob (a stiff string's B, up to NodeString::kMaxInharmonicity as its square). Its jawari is set each
	// sample (the process loop), with the ring's orbit.
	d.node.tune(hz, static_cast<double>(d.t60), kDroneHighLoss * std::pow(4.0, 1.0 - 2.0 * static_cast<double>(d.brightness)),
	            NodeString::kMaxInharmonicity * st * st);
}

void ResonateDsp::tuneDrone(const DroneResponse& resp, double offset) {
	const auto coeffFor = [&](double seconds) {
		const double samples = seconds * sr_;
		return samples < 1e-6 ? 1.0f : static_cast<float>(1.0 - std::exp(-1.0 / samples));
	};
	for (int i = 0; i < static_cast<int>(kMaxDroneStrings); ++i) {
		const size_t k = static_cast<size_t>(i);
		DroneString& d = strings_[k];
		const bool wanted = i < resp.count;
		const int want = resp.intervals[k];
		const double wantPitch = resp.pitch[k];
		const float wantDetune = resp.detuneCents[k];
		if (wanted && !d.active) {
			// A NEW STRING starts silent — its follower and its ear at 0 — so it needs no fade in.
			resetString(k);
			d.active = true;
			d.interval = want;
			d.pitch = wantPitch;
			d.detune = wantDetune;
			d.level = d.levelTarget = resp.level[k];
			d.root = d.pendingRoot = settings_.root;
		}
		else if (wanted && (d.pitch != wantPitch || d.detune != wantDetune)) {
			// A NEW NOTE FOR THIS SLOT (a chord or tuning change): a different string — its ear, level and scales — so the
			// old one fades out over 10 ms and the new one starts from silence (it takes the Root as it is then).
			d.retuning = true;
			d.leaving = false;
			d.pending = want;
			d.pendingPitch = wantPitch;
			d.pendingDetune = wantDetune;
			d.pendingRoot = settings_.root;
		}
		else if (wanted) {
			d.retuning = false; // wanted as it is: a retune asked for and taken back fades back in
			// A NEW ROOT RETUNES THE RINGING STRING IN PLACE (v11, 2026-10-03: a drone keeps ringing
			// through a root change): configureString, below, retunes its node string as it rings — the glide road
			// (NodeString::tune), MEASURED click-free — and its ear. v10's comb strings needed the fade (a jump in a ringing
			// loop's delay clicked: x8.1).
			d.root = d.pendingRoot = settings_.root;
			d.leaving = false;
		}
		else if (d.active) {
			d.retuning = true;
			d.leaving = true;
		}
		if (d.active) {
			// ITS OWN RESPONSE — while it fades out to retune, the slot's new one already (its ear is replaced
			// from silence when the retune lands, and its old sound is fading regardless).
			d.levelTarget = wanted ? resp.level[k] : 0.0f;
			if (wanted) {
				d.q = resp.q[k];
				d.t60 = resp.t60Seconds[k];
				d.brightness = resp.brightness[k];
				d.swellCoeff = coeffFor(static_cast<double>(resp.attackSeconds[k]));
				d.motionFall = coeffFor(static_cast<double>(resp.t60Seconds[k]) / kT60PerTau);
				// ITS SIDE, EQUAL POWER and unity at the centre: at Drone Width 0 both gains are exactly 1 (mono, as
				// before); otherwise sqrt2 x (cos, sin) of (1 + pan x width) pi/4, so L^2 + R^2 is always 2.
				const float w = std::clamp(std::isfinite(settings_.droneWidth) ? settings_.droneWidth : 0.0f, 0.0f, 1.0f);
				if (w == 0.0f) {
					d.panL = d.panR = 1.0f;
				}
				else {
					const double theta = (1.0 + static_cast<double>(resp.pan[k]) * w) * 0.25 * 3.14159265358979323846;
					d.panL = static_cast<float>(std::sqrt(2.0) * std::cos(theta));
					d.panR = static_cast<float>(std::sqrt(2.0) * std::sin(theta));
				}
			}
			configureString(d, offset);
		}
	}
}

bool ResonateDsp::anyDroneSounding() const {
	for (const DroneString& d : strings_) {
		if (d.sounding > 0.0f) {
			return true;
		}
	}
	return false;
}

// ============================================================================
// THE CYCLE, THE GOURD BODY AND THE TWIN GOURDS
// ============================================================================

namespace {
/// A φ-TRIANGLE's 0..1 across a range: geometric (Hz, Q) or linear (dB).
double geometric(const double (&range)[2], float t) {
	return range[0] * std::pow(range[1] / range[0], static_cast<double>(t));
}
double linear(const double (&range)[2], float t) {
	return range[0] + (range[1] - range[0]) * static_cast<double>(t);
}
} // namespace

ResonateDsp::Body ResonateDsp::bodyFor(double coordinate) {
	Body b;
	using owlet::dsp::phi::evalTriangle;
	b.peaks[0] = { geometric(kBodyLowHz, evalTriangle(coordinate, 1.0f, kPhiBodyLowHz)),
		           geometric(kBodyLowQ, evalTriangle(coordinate, 1.0f, kPhiBodyLowQ)),
		           linear(kBodyLowDb, evalTriangle(coordinate, 1.0f, kPhiBodyLowGain)) };
	b.peaks[1] = { geometric(kBodyWarmHz, evalTriangle(coordinate, 1.0f, kPhiBodyWarmHz)),
		           geometric(kBodyWarmQ, evalTriangle(coordinate, 1.0f, kPhiBodyWarmQ)),
		           linear(kBodyWarmDb, evalTriangle(coordinate, 1.0f, kPhiBodyWarmGain)) };
	b.peaks[2] = { geometric(kBodyUpperHz, evalTriangle(coordinate, 1.0f, kPhiBodyUpperHz)),
		           geometric(kBodyUpperQ, evalTriangle(coordinate, 1.0f, kPhiBodyUpperQ)),
		           linear(kBodyUpperDb, evalTriangle(coordinate, 1.0f, kPhiBodyUpperGain)) };
	b.twinOctaves = linear(kTwinOctaves, evalTriangle(coordinate, 1.0f, kPhiTwinSpread));
	return b;
}

CyclePattern ResonateDsp::cyclePatternAt(double coordinate) {
	const double z = std::floor(coordinate * static_cast<double>(kDroneZones));
	const int zone = static_cast<int>(z - std::floor(z / static_cast<double>(kDroneZones)) * static_cast<double>(kDroneZones));
	return static_cast<CyclePattern>(std::clamp(zone, 0, static_cast<int>(CyclePattern::kCount) - 1));
}

int ResonateDsp::cycleString(CyclePattern pattern, int count, int step) {
	const int n = std::max(1, count);
	if (n == 1) {
		return 0;
	}
	const int s = std::max(0, step);
	switch (pattern) {
	case CyclePattern::Tanpura: {
		// EVERY STRING ABOVE THE LOWEST IN ORDER, THEN THE LOWEST: Tanpura's 0, 7, 12, 12 plays Pa, Sa, Sa, low Sa.
		const int i = s % n;
		return i < n - 1 ? i + 1 : 0;
	}
	case CyclePattern::Up: return s % n;
	case CyclePattern::Down: return n - 1 - s % n;
	case CyclePattern::UpDown: {
		const int period = 2 * n - 2;
		const int i = s % period;
		return i < n ? i : period - i;
	}
	case CyclePattern::OutsideIn:
	case CyclePattern::InsideOut: {
		// 0, n-1, 1, n-2, ... — and Inside-Out is that read backwards.
		int i = s % n;
		if (pattern == CyclePattern::InsideOut) {
			i = n - 1 - i;
		}
		return (i % 2 == 0) ? i / 2 : n - 1 - i / 2;
	}
	case CyclePattern::Alternate: {
		// 0, 2, 4, ... then 1, 3, 5, ...
		const int i = s % n;
		const int evens = (n + 1) / 2;
		return i < evens ? 2 * i : 2 * (i - evens) + 1;
	}
	case CyclePattern::PhiWalk: {
		const double x = static_cast<double>(s) * 0.6180339887498949;
		return std::min(n - 1, static_cast<int>((x - std::floor(x)) * static_cast<double>(n)));
	}
	case CyclePattern::kCount: break;
	}
	return 0;
}

float ResonateDsp::cycleRingPluckPos(int interval) {
	return owlet::dsp::phi::evalTriangle(static_cast<double>(interval) * static_cast<double>(kPhiDroneStringSpread), 1.0f,
	                                      kPhiCycleRingPos);
}

float ResonateDsp::cycleVelocity(double coordinate, int step) {
	const double phase = coordinate * static_cast<double>(kDroneZones)
	                     + static_cast<double>(step) * static_cast<double>(kPhiDroneStringSpread);
	return 0.5f + 0.5f * owlet::dsp::phi::evalTriangle(phase, 1.0f, kPhiCycleVelocity);
}

int ResonateDsp::pluckLead() const {
	// THE STRIKE'S MIDDLE, THEN THE MIDDLE OF WHAT IT MAKES AT THE PICKUP: a push at the drive point d reaches the pickup p
	// directly after |p - d| of a length and is undone by its reflection off the near end after p + d, at 2 f lengths a
	// second; between the two the pickup holds a plateau whose middle is max(p, d) (MEASURED by the grid case at
	// C2, C3 and C5).
	const double coord = std::isfinite(settings_.droneCycle) ? static_cast<double>(settings_.droneCycle) : 0.0;
	int count = 0;
	for (const DroneString& d : strings_) {
		count += (d.active && !d.leaving) ? 1 : 0;
	}
	if (count == 0) {
		return 0;
	}
	const DroneString& d = strings_[static_cast<size_t>(cycleString(cyclePatternAt(coord), count, cycleStep_))];
	const double half = 0.5 * std::lround(kDroneStrikeSeconds * sr_);
	const double travel = d.inc > 0.0 ? std::max(d.node.pickPosition(), NodeString::kDrivePos) / (2.0 * d.inc) : 0.0;
	return static_cast<int>(std::lround(half + travel));
}

void ResonateDsp::cyclePluck(int lead) {
	// A PLUCK STILL WAITING FOR ITS GRID is delivered now rather than lost (only possible when a division is shorter than
	// its lead; never, while the lead is 0).
	if (pendingPluck_.countdown >= 0) {
		deliverCyclePluck();
	}
	const double coord = std::isfinite(settings_.droneCycle) ? static_cast<double>(settings_.droneCycle) : 0.0;
	int count = 0;
	for (const DroneString& d : strings_) {
		count += (d.active && !d.leaving) ? 1 : 0;
	}
	if (count == 0) {
		return;
	}
	const int k = cycleString(cyclePatternAt(coord), count, cycleStep_);
	const float velocity = cycleVelocity(coord, cycleStep_);
	++cycleStep_;
	// THE STRING, STRUCK (v11): at its drive point, `lead` samples ahead of the grid so it peaks on it (pluckLead()).
	strings_[static_cast<size_t>(k)].node.strike(kCyclePluckAmp * velocity * cycleGain(),
	                                            std::max(1, static_cast<int>(std::lround(kDroneStrikeSeconds * sr_))));
	pendingPluck_ = { std::max(0, lead), k, velocity };
	if (lead <= 0) {
		deliverCyclePluck();
	}
}

void ResonateDsp::deliverCyclePluck() {
	const int k = pendingPluck_.string;
	const float velocity = pendingPluck_.velocity;
	pendingPluck_.countdown = -1;
	DroneString& d = strings_[static_cast<size_t>(k)];
	// AND IT PLUCKS THE RING, as a played note does (an input onset did until v19) ("the arps don't seem to excite the string, which
	// makes the drone timbre more static"): a Cycle pluck that only fed the comb strings left the ring still, so its
	// shape, its jawari, the scanned voice and the note heads never moved. At a place on the ring's path each string
	// has its own of (its interval through a φ-triangle). The ring's own queue merges several in one tick, as it does a note's.
	// v13: AS HARD AS PLUCK SAYS, and no longer Cycle Level (which now plays only the drone's strings): step x
	// kCycleRingPluck x pluckGain — at the default Pluck and Cycle Level the old amount, bit for bit.
	// v20: x DRONE PLUCK, not Pluck, which is now the played notes' alone; at its middle the old amount, bit for bit.
	const float ringAmp = kCycleRingPluck * velocity * pluckGain(settings_.dronePluck);
	if (ringAmp > 0.0f && cycleRingPluckOn_) {
		lastPluckPos_ = cycleRingPluckPos(d.interval);
		ring_.pluck(ringAmp, lastPluckPos_);
		lastRingPluckAmp_ = ringAmp;
		for (Head& h : heads_) { // v19: every head, as hard as its ring pluck, x kCycleBurst (not a fixed 1)
			h.drivePluck = kCycleBurst * ringAmp / kPluckMax;
		}
		pluckAge_ = 0.0f;
		excitedThisWindow_ = playedThisWindow_ = true;
		++cycleRingPlucks_;
	}
	pluckLog_[static_cast<size_t>(pluckLogCount_) % pluckLog_.size()] = { sampleClock_, k, velocity };
	lastPluckAt_ = sampleClock_;
	++pluckLogCount_;
}

float ResonateDsp::Biquad::run(float x, int ch) {
	const float y = b0 * x + z1[ch];
	z1[ch] = b1 * x - a1 * y + z2[ch];
	z2[ch] = b2 * x - a2 * y;
	if (std::abs(z1[ch]) < 1e-20f && std::abs(z2[ch]) < 1e-20f) {
		z1[ch] = z2[ch] = 0.0f; // no denormal tail: a rung-out body reaches exact zero
	}
	return y;
}

namespace {
/// RBJ PEAKING at `hz`, Q `q`, `db` decibels.
void peakingInto(ResonateDsp::BodyPeak p, double sampleRate, float& b0, float& b1, float& b2, float& a1, float& a2) {
	const double pi = 3.14159265358979323846;
	const double w0 = 2.0 * pi * std::min(p.hz, 0.45 * sampleRate) / sampleRate;
	const double A = std::pow(10.0, p.db / 40.0);
	const double alpha = std::sin(w0) / (2.0 * p.q);
	const double a0 = 1.0 + alpha / A;
	b0 = static_cast<float>((1.0 + alpha * A) / a0);
	b1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
	b2 = static_cast<float>((1.0 - alpha * A) / a0);
	a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
	a2 = static_cast<float>((1.0 - alpha / A) / a0);
}
/// The body's peaks at Body `amount`, shifted by `octaves`.
ResonateDsp::BodyPeak shifted(ResonateDsp::BodyPeak p, double amount, double octaves) {
	return { p.hz * std::exp2(octaves), p.q, amount * p.db };
}
} // namespace

double ResonateDsp::bodyPower(const Body& b, double amount, double octaves, double sampleRate, double hz) {
	const double pi = 3.14159265358979323846;
	const std::complex<double> z = std::polar(1.0, -2.0 * pi * std::min(hz, 0.45 * sampleRate) / sampleRate);
	std::complex<double> h = 1.0;
	for (const BodyPeak& raw : b.peaks) {
		float b0, b1, b2, a1, a2;
		peakingInto(shifted(raw, amount, octaves), sampleRate, b0, b1, b2, a1, a2);
		h *= (static_cast<double>(b0) + static_cast<double>(b1) * z + static_cast<double>(b2) * z * z)
		     / (1.0 + static_cast<double>(a1) * z + static_cast<double>(a2) * z * z);
	}
	return std::norm(h);
}

double ResonateDsp::bodyMakeup(const Body& b, double amount, double octaves, double sampleRate, double topHz) {
	const double pi = 3.14159265358979323846;
	double sum = 0.0;
	constexpr int kPoints = 64;
	for (int k = 0; k < kPoints; ++k) {
		const double hz = 60.0 * std::pow(topHz / 60.0, (k + 0.5) / kPoints);
		const std::complex<double> z = std::polar(1.0, -2.0 * pi * hz / sampleRate);
		std::complex<double> h = 1.0;
		for (const BodyPeak& raw : b.peaks) {
			float b0, b1, b2, a1, a2;
			peakingInto(shifted(raw, amount, octaves), sampleRate, b0, b1, b2, a1, a2);
			h *= (static_cast<double>(b0) + static_cast<double>(b1) * z + static_cast<double>(b2) * z * z)
			     / (1.0 + static_cast<double>(a1) * z + static_cast<double>(a2) * z * z);
		}
		sum += std::norm(h);
	}
	return 1.0 / std::sqrt(sum / kPoints);
}

void ResonateDsp::updateBody() {
	const float amount = clamp01(settings_.droneBody);
	const float twin = clamp01(settings_.droneTwin);
	if (!(amount > 0.0f)) {
		if (bodyOn_) {
			clearBody();
		}
		bodyOn_ = twinOn_ = false;
		bodyKey_[0] = -1.0;
		return;
	}
	const double coord = std::isfinite(settings_.droneCharacter)
	                         ? std::clamp(static_cast<double>(settings_.droneCharacter), -kDroneCoordMax, kDroneCoordMax)
	                         : kDroneCharacterDefault;
	bodyOn_ = true;
	twinOn_ = twin > 0.0f;
	// WHAT THE DRONE'S STRINGS SOUND, for its level match (below): a signature of their pitches and levels.
	double drone = 0.0;
	for (size_t k = 0; k < kMaxDroneStrings; ++k) {
		const DroneString& d = strings_[k];
		if (d.active) {
			drone += (d.inc * 1e3 + static_cast<double>(d.levelTarget)) * static_cast<double>(k + 1);
		}
	}
	// BUILT ONLY WHEN SOMETHING MOVED (the coefficients and the level match are 400-odd complex products).
	const bool shape = !(coord == bodyKey_[0] && static_cast<double>(amount) == bodyKey_[1] && static_cast<double>(twin) == bodyKey_[2]);
	if (!shape && drone == bodyKey_[3]) {
		return;
	}
	bodyKey_[0] = coord;
	bodyKey_[1] = amount;
	bodyKey_[2] = twin;
	bodyKey_[3] = drone;
	const Body b = bodyFor(coord);
	for (int ch = 0; ch < 2; ++ch) {
		// THE TWIN GOURDS: the left gourd below the centre, the right above, by Twin x the spread.
		const double octaves = (ch == 0 ? -1.0 : 1.0) * static_cast<double>(twin) * b.twinOctaves;
		if (shape) {
			for (size_t i = 0; i < b.peaks.size(); ++i) {
				for (BodyBank* bank : { &bodyDrone_, &bodyTone_ }) {
					Biquad& q = (*bank)[static_cast<size_t>(ch)][i];
					peakingInto(shifted(b.peaks[i], amount, octaves), sr_, q.b0, q.b1, q.b2, q.a1, q.a2);
				}
			}
			// LEVEL MATCHED: Body (and Twin) colour the sound, they do not make it louder. The notes' strings carry less
			// above a few kHz than a broad match assumes, so theirs is matched over 60 Hz..3 kHz: MEASURED within 1.5 dB of
			// Body 0, Twin 0 and 1, where one match over 60 Hz..8 kHz left the notes 1.7 dB up.
			bodyToneMakeup_[ch] = static_cast<float>(bodyMakeup(b, amount, octaves, sr_, 3000.0));
		}
		// THE DRONE'S IS MATCHED AT ITS OWN STRINGS (v11 item 6): its node strings put nearly all their energy in their
		// first partials, and the body's low resonance sits among them, so a broad match left it louder by up to the peak's
		// gain — MEASURED +3.6 dB at Body 1 over 60 Hz..8 kHz, still +2.0 over 60..300 Hz. So the mean power gain is taken
		// at each sounding string's partials 1..4, weighted by its level squared over h^4 (a plucked string's falling
		// partials); with no string tuned, over 60 Hz..8 kHz.
		double num = 0.0;
		double den = 0.0;
		for (size_t k = 0; k < kMaxDroneStrings; ++k) {
			const DroneString& d = strings_[k];
			if (!d.active || !(d.inc > 0.0)) {
				continue;
			}
			const double level = std::max(static_cast<double>(d.levelTarget), 1e-6);
			for (int h = 1; h <= 4; ++h) {
				const double w = level * level / static_cast<double>(h * h * h * h);
				num += w * bodyPower(b, amount, octaves, sr_, d.inc * sr_ * h);
				den += w;
			}
		}
		bodyMakeup_[ch] = static_cast<float>(den > 0.0 ? 1.0 / std::sqrt(num / den) : bodyMakeup(b, amount, octaves, sr_, 8000.0));
	}
}

void ResonateDsp::clearBody() {
	for (BodyBank* bank : { &bodyDrone_, &bodyTone_ }) {
		for (auto& channel : *bank) {
			for (Biquad& q : channel) {
				q.z1[0] = q.z1[1] = q.z2[0] = q.z2[1] = 0.0f;
			}
		}
	}
}

void ResonateDsp::bodyStep(float& l, float& r, BodyBank& bank, const float (&makeup)[2]) {
	for (Biquad& q : bank[0]) {
		l = q.run(l, 0);
	}
	for (Biquad& q : bank[1]) {
		r = q.run(r, 0);
	}
	l *= makeup[0];
	r *= makeup[1];
}

void ResonateDsp::buildKernel(const float* path, float rotate, float length, float* out) {
	const int taps = std::clamp(tapsFor(length), 1, kMaxTaps);
	constexpr int n = WeaveString::kNodes;
	float p[n];
	float mean = 0.0f;
	for (int j = 0; j < n; ++j) {
		// The path read `rotate` of a cycle further round, linearly between points.
		const float u = (static_cast<float>(j) / n + rotate) * n;
		const float fl = std::floor(u);
		const int i0 = static_cast<int>(fl) % n;
		const float f = u - fl;
		p[j] = path[(i0 + n) % n] + (path[(i0 + 1 + n) % n] - path[(i0 + n) % n]) * f;
		mean += p[j];
	}
	mean /= n;
	double energy = 0.0; // only to recognise an all-zero kernel
	for (int t = 0; t < taps; ++t) {
		// Tap t reads one cycle of the (DC-free) path stretched over `length` taps, times a Hann window over the
		// same length: at a whole-number length exactly the old recipe, and continuous between them.
		const float u = static_cast<float>(t) * n / length;
		const int i0 = static_cast<int>(u);
		const float f = u - static_cast<float>(i0);
		const float v = (p[i0 % n] - mean) + ((p[(i0 + 1) % n] - mean) - (p[i0 % n] - mean)) * f;
		// portableCos, not the C library's cosf: macOS's rounds one of these 64 window arguments differently (backlog LP).
		const float hann = 0.5f - 0.5f * dsp::portableCos(kTwoPi * (static_cast<float>(t) + 0.5f) / length);
		out[t] = v * hann;
		energy += static_cast<double>(out[t]) * out[t];
	}
	// NORMALISED BY ITS PEAK MAGNITUDE RESPONSE, so the filter never boosts any frequency above unity. Unit
	// energy (the plan's first recipe) boosted a periodic input matched to the kernel — up to +11 dB
	// measured at max Drive — and unit L1 never boosts but is needlessly quiet everywhere.
	const float peak = (energy > 1e-24 && fft_ != nullptr) ? peakMagnitude(out, taps) : 0.0f;
	// kPeakMargin covers what a parabola through three bins can still miss (MEASURED worst 1.0002 against a
	// grid eight times finer), so the gain is at most 1, strictly — Requirement: the filter never boosts.
	const float norm = (peak > 1e-12f) ? 1.0f / (peak * kPeakMargin) : 0.0f;
	for (int t = 0; t < taps; ++t) {
		out[t] *= norm;
	}
	for (int t = taps; t < kMaxTaps; ++t) {
		out[t] = 0.0f;
	}
}

void ResonateDsp::buildKernels() {
	const float rotateR = kWidthCycles * width_.value();
	buildKernel(ring_.smoothedNodes(), 0.0f, kernelLength_, kernel_[1][0].data());
	if (rotateR == 0.0f) {
		kernel_[1][1] = kernel_[1][0]; // WIDTH 0: the right channel's kernel is the left's, bit for bit
	}
	else {
		buildKernel(ring_.smoothedNodes(), rotateR, kernelLength_, kernel_[1][1].data());
	}
}

void ResonateDsp::seatKernels() {
	kernelLength_ = sizeLength();
	kernelTaps_ = kernelTapsFrom_ = tapsFor(kernelLength_);
	buildKernels();
	kernel_[0] = kernel_[1];
	kernelStale_ = false;
}

void ResonateDsp::firAhead(const float* left, const float* right, int count) {
	// THE NEXT `count` SAMPLES' HISTORY (the first, this sample's, the loop has written), as the loop writes it.
	for (int d = 1; d < count; ++d) {
		const size_t p = static_cast<size_t>((historyPos_ + d) & (kHistory - 1));
		history_[0][p] = history_[0][p + kHistory] = finiteOr0(left[d]);
		history_[1][p] = history_[1][p + kHistory] = finiteOr0(right[d]);
	}
	// Over the LONGER of the two kernels: when Size shrinks, the outgoing kernel's dropped taps fade too.
	const int taps = std::max(kernelTaps_, kernelTapsFrom_);
	const float* fromL = kernel_[0][0].data();
	const float* toL = kernel_[1][0].data();
	const float* fromR = kernel_[0][1].data();
	const float* toR = kernel_[1][1].data();
	int d = 0;
	while (d < count) {
		const int p = (historyPos_ + d) & (kHistory - 1);
		// The newest sample and the ones before it, contiguous in the mirror: newest[-j] is j samples back.
		const float* newestL = history_[0].data() + p + kHistory;
		const float* newestR = history_[1].data() + p + kHistory;
		if (d + 4 <= count && p + 4 <= kHistory) {
			// FOUR SAMPLES, ONE TO A LANE: lane s sums tap j of sample d + s, which is newest[s - j] — each lane in the scalar
			// loop's order from 0, so each is that loop's sum to the bit (SSE2 adds and multiplies, -ffp-contract=off).
			__m128 aL = _mm_setzero_ps();
			__m128 bL = _mm_setzero_ps();
			__m128 aR = _mm_setzero_ps();
			__m128 bR = _mm_setzero_ps();
			for (int j = 0; j < taps; ++j) {
				const __m128 xL = _mm_loadu_ps(newestL - j);
				const __m128 xR = _mm_loadu_ps(newestR - j);
				aL = _mm_add_ps(aL, _mm_mul_ps(_mm_set1_ps(fromL[j]), xL));
				bL = _mm_add_ps(bL, _mm_mul_ps(_mm_set1_ps(toL[j]), xL));
				aR = _mm_add_ps(aR, _mm_mul_ps(_mm_set1_ps(fromR[j]), xR));
				bR = _mm_add_ps(bR, _mm_mul_ps(_mm_set1_ps(toR[j]), xR));
			}
			_mm_storeu_ps(firFrom_[0].data() + d, aL);
			_mm_storeu_ps(firTo_[0].data() + d, bL);
			_mm_storeu_ps(firFrom_[1].data() + d, aR);
			_mm_storeu_ps(firTo_[1].data() + d, bR);
			d += 4;
			continue;
		}
		// ONE SAMPLE (the window's last few, or four that would straddle the mirror's end), as the loop always summed it.
		float aL = 0.0f;
		float bL = 0.0f;
		float aR = 0.0f;
		float bR = 0.0f;
		for (int j = 0; j < taps; ++j) {
			const float xL = newestL[-j];
			const float xR = newestR[-j];
			aL += fromL[j] * xL;
			bL += toL[j] * xL;
			aR += fromR[j] * xR;
			bR += toR[j] * xR;
		}
		const size_t at = static_cast<size_t>(d);
		firFrom_[0][at] = aL;
		firTo_[0][at] = bL;
		firFrom_[1][at] = aR;
		firTo_[1][at] = bR;
		++d;
	}
}

void ResonateDsp::onTick() {
	// THE RING'S ORBIT (v10): how far it has swung from rest under the bow, either way, over the largest it has been lately
	// (held over kProximityHoldSeconds), 0..1 — the radius of the main string's orbit, the "eccentric radius vector which
	// brings the string closer to the bridge" — read now, and eased toward across the next window. (Only the swing toward
	// the ring's own line, below rest, was the first reading; a bowed ring leans one way, so it read ~0.02 on average.)
	{
		const float dep = ring_.bowDeparture();
		const float release = static_cast<float>(std::exp(-static_cast<double>(ring_.tickLength()) / (kProximityHoldSeconds * sr_)));
		proxRef_ = std::max(std::abs(dep), proxRef_ * release);
		proxPrev_ = proxTarget_;
		proxTarget_ = proxRef_ > 1e-9f ? std::clamp(std::abs(dep) / proxRef_, 0.0f, 1.0f) : 0.0f;
	}
	// THE GATE: how far the string is from rest, as an amplitude — full at kGateEnergyRef, and exactly
	// zero below kGateFloor of that, reached continuously.
	// FROM THE ENERGY'S ENVELOPE, NOT THE ENERGY: (x - rest)^2 + v^2 is not conserved as the string swings,
	// so it wobbles tick to tick, and a gate read straight off it closed for a block and opened again as a
	// decaying tail crossed the floor — an exactly silent block mid-tail, which is what a host sleeps on
	// (test_clap_render caught it). The envelope rises with the energy and falls no faster than
	// kGateRelease a tick, so once nothing is exciting the string it crosses the floor once.
	gateEnergy_ = std::max(std::max(ring_.motionEnergy(), 0.0f), gateEnergy_ * kGateRelease);
	const float g = std::min(1.0f, std::sqrt(gateEnergy_ / kGateEnergyRef));
	gatePrev_ = gateTarget_;
	const float open = (g > kGateFloor) ? (g - kGateFloor) / (1.0f - kGateFloor) : 0.0f;
	// AND ONCE SHUT WITH NOTHING EXCITING THE STRING, IT STAYS SHUT until something does: an envelope can
	// still dip under the floor between two swings of a slow mode and rise over it at the next. Excitation
	// is any non-zero input sample in the window, the Bow up, or a pluck.
	if (excitedThisWindow_) {
		gateLatched_ = false;
	}
	else if (open == 0.0f) {
		gateLatched_ = true;
	}
	gateTarget_ = gateLatched_ ? 0.0f : open;
	excitedThisWindow_ = false;
	// PLAYED, NOT ONLY BOWED (v14): what the drone's scanned voice follows the string's motion under. The Bow keeps the ring moving
	// for ever with nothing played, and a drone that followed that rang for ever too (test_clap_render: never slept at the default
	// Bow); the input, a pluck, or the puck moving is a player.
	played_ = playedThisWindow_;
	playedThisWindow_ = false;

	// THE KERNEL THE NEXT WINDOW FADES TO, from the path the tick just built — unless Blend sits at 0, where
	// the filter is not heard: then nothing is built, and the kernels are seated afresh when Blend moves.
	if (blendIdle()) {
		kernelStale_ = true;
	}
	else {
		kernelTapsFrom_ = kernelTaps_;
		kernelLength_ = sizeLength();
		kernelTaps_ = tapsFor(kernelLength_);
		kernel_[0] = kernel_[1];
		buildKernels();
	}
	pluckAge_ = std::min(pluckAge_ + 1.0f, kPluckAgeMax);
	publish();
}

void ResonateDsp::publish() {
	if (sink_.publish == nullptr) {
		return;
	}
	Reading r;
	r.model = static_cast<float>(static_cast<int>(ring_.model()));
	const int n = ring_.nodeCount();
	for (int i = 0; i < n; ++i) {
		r.nodes[static_cast<size_t>(i)] = ring_.modelNodes()[i];
		r.rest[static_cast<size_t>(i)] = ring_.restNodes()[i];
	}
	const float travel = ring_.travelPhaseAt(0);
	// THE PITCH EACH HEAD SOUNDS, in semitones: its note (the drone's Root; Mono's glide, mid-glide) plus the
	// octave, semitone and fine offset — the same number its phase increment is made from.
	const double offset = semitonesOffset();
	// THE NOTE HEADS, every one that sounds (there are never more than the picture's eight).
	size_t shown = 0;
	for (const Head& h : heads_) {
		if (h.live) {
			r.heads[shown] = static_cast<float>(wrap01(h.phase + travel));
			r.headGains[shown] = h.gain;
			r.headNotes[shown] = static_cast<float>(h.semis + offset);
			++shown;
		}
	}
	// THE DRONE'S STRINGS, apart from the note heads: every string the chord has, sounding or not.
	const DroneResponse& resp = droneResponse();
	r.droneCount = static_cast<float>(resp.count);
	for (size_t i = 0; i < kMaxDroneStrings; ++i) {
		const DroneString& d = strings_[i];
		if (d.active) {
			r.droneHeads[i] = static_cast<float>(wrap01(d.phase + travel));
			r.droneGains[i] = d.sounding;
		}
		if (static_cast<int>(i) < resp.count) {
			r.droneNotes[i] = static_cast<float>(settings_.root + resp.pitch[i] + resp.detuneCents[i] / 100.0 + offset);
		}
	}
	r.travel = travel;
	r.pluckPos = lastPluckPos_;
	r.pluckAge = pluckAge_;
	r.energy = ring_.motionEnergy();
	r.inLevel = meterLevel_;
	// PARKED IS THE REST LOGIC'S OWN STATE, so the card can draw a frozen string as held rather than stuck.
	r.parked = resting_ ? 1.0f : 0.0f;
	packReading(r, packed_);
	sink_.publish(sink_.context, packed_.data(), packed_.size());
}

void ResonateDsp::process(float* left, float* right, int frames) {
	if (restartPending_) {
		restart();
	}
	applySettings(false);
	if (settings_.voicing != lastVoicing_) {
		lastVoicing_ = settings_.voicing;
		revoice();
	}
	applyNotes();

	const bool mono = settings_.voicing == static_cast<int>(Voicing::Mono);
	const double offset = semitonesOffset();
	// THE DRONE'S STRINGS, as the Chord asks for them this block: each at Root + its interval, heard through a
	// harmonic ear at its pitch, its feed swelling in over Drone Attack, ringing for Drone Decay (its T60), both
	// scaled by the zone's position.
	tuneDrone(droneResponse(), offset);
	const float excite = std::clamp(std::isfinite(settings_.droneExcite) ? settings_.droneExcite : 0.0f, 0.0f, 1.0f);
	// DRONE INPUT: the fader law, (2 v)^2, unity at the centre, followed per sample by a one-pole over kDroneInputSeconds
	// (it is modulatable; a per-sample pole, not a per-block ramp, so how the host cuts its blocks changes nothing — the fx
	// devices' partition sweep). At rest the gain IS its target and the pole moves it by exactly 0, so the centre is the input
	// bit for bit.
	const float inputTarget = [&] {
		const float v = std::clamp(std::isfinite(settings_.droneInput) ? settings_.droneInput : 0.5f, 0.0f, 1.0f);
		return (2.0f * v) * (2.0f * v);
	}();
	const float inputCoeff = 1.0f - std::exp(-1.0f / (kDroneInputSeconds * static_cast<float>(sr_)));
	// THE NOTES' STRINGS: their two controls' targets and pole, and their T60 (Ring Decay: kVoiceStringT60 x 2^value).
	const float blendTarget = std::clamp(std::isfinite(settings_.stringBlend) ? settings_.stringBlend : 0.0f, 0.0f, 1.0f);
	const float exciteTarget = std::clamp(std::isfinite(settings_.stringExcite) ? settings_.stringExcite : 0.0f, 0.0f, 1.0f);
	const float stringCoeff = 1.0f - std::exp(-1.0f / (kStringSeconds * static_cast<float>(sr_)));
	const double stringT60 = kVoiceStringT60 * std::exp2(static_cast<double>(std::clamp(std::isfinite(settings_.ringDecay) ? settings_.ringDecay : 0.0f, -2.0f, 2.0f)));
	const float fadeStep = 1.0f / std::max(1.0f, kRetuneSeconds * static_cast<float>(sr_));
	const float levelCoeff = 1.0f - std::exp(-1.0f / (kStringLevelSeconds * static_cast<float>(sr_)));
	const float voice = std::clamp(std::isfinite(settings_.droneVoice) ? settings_.droneVoice : 0.0f, 0.0f, 1.0f);
	// THE SWELL'S PRESENCE ENVELOPE lets go over 50 ms and the swell itself over kSwellFallSeconds; the picture's envelope of
	// each voice over 50 ms.
	const float swellRelease = std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));
	const float swellFall = 1.0f - std::exp(-1.0f / (kSwellFallSeconds * static_cast<float>(sr_)));
	const float envRelease = std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));
	// THE BRIDGE'S SPRING (to the soundboard) as a step's (2 pi f k)^2, and a rigid bridge's fall back to rest.
	const double bridgeSpring = (2.0 * 3.14159265358979 * kBridgeHz / sr_) * (2.0 * 3.14159265358979 * kBridgeHz / sr_);
	const double bridgeRest = std::exp(-1.0 / (kBridgeRestSeconds * sr_));
	// TRANSPORT ONLY (v11): the Cycle does nothing while the host is stopped, and so it keeps no device awake then.
	const bool cycleOn = cycleRunning() && (settings_.droneCycleTransport == 0 || clock_.playing);
	const double cyclePeriod = cyclePeriodBeats(settings_.droneCycleSync);
	const double secondsPerBeat = clock_.haveTempo() ? clock_.secondsPerBeat : 1.0;
	const bool cycleLocked = clock_.playing && clock_.haveTempo() && clock_.havePositionBeats();
	const int cycleInterval = static_cast<int>(std::max(1.0, std::round(cyclePeriod * secondsPerBeat * sr_)));
	updateBody();
	for (Head& h : heads_) {
		h.inc = hzOf(h.semis + h.bend + offset) / sr_; // the pitch the head has reached, never its target's
	}
	const float attackStep = 1.0f / std::max(1.0f, kHeadAttackSeconds * static_cast<float>(sr_));
	const float releaseStep = 1.0f / std::max(1.0f, kHeadReleaseSeconds * static_cast<float>(sr_));
	// A RELEASED HEAD'S HOLD falls at the ring's own rate: 60 dB in the strings' T60 (Ring Decay's), per sample.
	const float holdDecay = static_cast<float>(std::exp(-6.907755278982137 / (stringT60 * sr_)));
	const float drivePluckFall = std::exp(-1.0f / (kDrivePluckSeconds * static_cast<float>(sr_)));
	const float driveCapFall = std::exp(-1.0f / (kDriveCapRelease * static_cast<float>(sr_)));
	const float feedBoundCoeff = 1.0f - std::exp(-1.0f / (kDroneFeedSeconds * static_cast<float>(sr_)));
	const float stealStep = 1.0f / std::max(1.0f, kStealSeconds * static_cast<float>(sr_));
	// MONO'S GLIDE: a one-pole in semitones per sample; at Porta's minimum it snaps.
	const bool snapGlide = settings_.porta <= static_cast<float>(kPortaLog2Min) + 1e-6f;
	const double glideCoeff =
	    snapGlide ? 1.0 : 1.0 - std::exp(-1.0 / (std::exp2(static_cast<double>(settings_.porta)) * sr_));
	const double bendCoeff = 1.0 - std::exp(-1.0 / (kBendSeconds * sr_));
	const int restSamples = static_cast<int>(kRestSilentSeconds * sr_);

	// MONO'S GLIDE, one sample: every sample whether the string is parked or not, so where it wakes
	// changes nothing about the pitch. It moves a head's pitch toward its key; in Poly nothing glides, so a
	// head released mid-glide by a change to Poly keeps the pitch it had reached.
	// AND EVERY HEAD'S BEND (v18, MPE): toward its key's tuning expression on a one-pole of kBendSeconds, in any Voicing, so a
	// host's block-rate expression steps reach the pitch (and the head's string, retuned per sample) with no step; landing on it
	// exactly, so a bend released to 0 is the unbent pitch to the bit.
	auto glide = [&]() {
		for (Head& h : heads_) {
			if (!h.live || h.stealing) {
				continue;
			}
			bool moved = false;
			if (mono && h.semis != h.key) {
				const double to = static_cast<double>(h.key);
				h.semis = (snapGlide || std::abs(to - h.semis) < kGlideLandSemis) ? to : h.semis + (to - h.semis) * glideCoeff;
				moved = true;
			}
			const double bendTo = keyTuning_[static_cast<size_t>(std::clamp(h.key, 0, 127))];
			if (h.bend != bendTo) {
				h.bend = std::abs(bendTo - h.bend) < kBendLandSemis ? bendTo : h.bend + (bendTo - h.bend) * bendCoeff;
				moved = true;
			}
			if (moved) {
				h.inc = hzOf(h.semis + h.bend + offset) / sr_;
			}
		}
	};
	auto stepHead = [&](Head& h) {
		if (!h.live) {
			return;
		}
		if (h.stealing) {
			// THE STEAL'S FADE: out fast, then the new note from phase 0 — never a phase jump at gain.
			h.gain = std::max(0.0f, h.gain - stealStep);
			h.phase = wrap01(h.phase + h.inc);
			if (h.gain <= 0.0f) {
				const bool keep = h.nextHeld;
				startHead(h, h.nextKey);
				h.held = keep;
				h.live = keep;
			}
			return;
		}
		// RING: a released head stays open until the ring has come to rest or its hold has fallen kRingCloseLevel under its
		// start, then releases on the normal ramp. While it rings its gain stays where it was: its hold does the falling.
		if (h.ringing && (ringStill() || h.hold < kRingCloseLevel * h.holdRef)) {
			h.ringing = false;
		}
		const bool open = h.held || h.ringing;
		if (!(h.ringing && !h.held)) {
			h.gain = open ? std::min(1.0f, h.gain + attackStep) : std::max(0.0f, h.gain - releaseStep);
		}
		if (h.released) {
			h.hold *= holdDecay;
		}
		h.phase = wrap01(h.phase + h.inc);
		if (!open && h.gain <= 0.0f) {
			h.live = false;
		}
	};

	int done = 0;
	while (done < frames) {
		// PARKED: exact zero out, nothing ticking, until the input or the Bow wakes it. Heads and ramps
		// still move per sample, so where it wakes changes nothing about what it then does.
		while (resting_ && done < frames) {
			const float inL = finiteOr0(left[done]);
			const float inR = finiteOr0(right[done]);
			// A HEAD WAKES IT only if the tone would carry something: Sustain up, or a string still moving.
			// MORPH MOVING wakes it too: its motion bows the string.
			if (cycleOn || inL != 0.0f || inR != 0.0f || settings_.bow > 0.0f || !morph_.settled() || morph_.value() != parkedMorph_
			    || (anyNoteHeadLive() && (sustain_.value() > 0.0f || !ringStill()))) {
				resting_ = false;
				idleSamples_ = 0;
				break;
			}
			for (dyn::LinearRamp* r : ramps_) {
				r->next();
			}
			glide();
			for (DroneString& d : strings_) {
				d.phase = wrap01(d.phase + d.inc); // silent while parked (its follower is at 0), still turning
			}
			for (Head& h : heads_) {
				stepHead(h);
			}
			if (tap_ != nullptr) { // parked: no dry (it would have woken the string), no wet, the ramps where they stand
				const auto at = static_cast<size_t>(done);
				tap_->dry[0][at] = tap_->dry[1][at] = 0.0f;
				tap_->tone[0][at] = tap_->tone[1][at] = 0.0f;
				tap_->drone[0][at] = tap_->drone[1][at] = 0.0f;
				tap_->mix[at] = mix_.value();
				tap_->enable[at] = enable_.value();
			}
			left[done] = 0.0f;
			right[done] = 0.0f;
			++done;
			++sampleClock_;
			// THE PICTURE KEEPS ITS CADENCE WHILE PARKED: nothing ticks, but heads open, close and glide, and
			// the card must see them; a picture per tick-length of parked samples, counted per sample.
			if (++restPublishCount_ >= ring_.tickLength()) {
				restPublishCount_ = 0;
				meterLevel_ = 0.0f;
				pluckAge_ = std::min(pluckAge_ + 1.0f, kPluckAgeMax);
				publish();
			}
		}
		if (done >= frames) {
			break;
		}

		const int untilTick = ring_.samplesUntilTick();
		const int m = std::min(frames - done, untilTick);
		const int len = ring_.tickLength();
		const int windowAt = len - untilTick;
		for (int k = 0; k < m; ++k) {
			fade_[static_cast<size_t>(k)] = ring_.fadeAt(k);
		}
		// THE FIR'S LOOK-AHEAD in this window (firAhead): its sums cover samples [firBegin, firEnd), and the history is written
		// up to historyAhead.
		int firBegin = 0;
		int firEnd = 0;
		int historyAhead = 0;
		for (int k = 0; k < m; ++k) {
			const int i = done + k;
			const float dryL = finiteOr0(left[i]);
			const float dryR = finiteOr0(right[i]);
			follower_.process(dryL, dryR);
			if (dryL != 0.0f || dryR != 0.0f) {
				excitedThisWindow_ = playedThisWindow_ = true;
			}
			const float blend = blend_.next();
			const float levelGain = levelGain_.next();
			const float mix = mix_.next();
			const float width = width_.next();
			const float enable = enable_.next();
			morph_.next();
			const float sustain = sustain_.next();
			const float droneOut = droneOut_.next();
			const float sympathy = sympathy_.next();
			const float droneSympathy = droneSympathy_.next();

			// THE WINDOW'S POSITION, the same fraction the string's own fade and travel use.
			const float t = static_cast<float>(windowAt + k + 1) / static_cast<float>(len);
			gate_ = gatePrev_ + (gateTarget_ - gatePrev_) * t;

			// THE TONE: every sounding head reads the whole string, R a quarter-cycle times Width round.
			const WeaveString::Fade& f = fade_[static_cast<size_t>(k)];
			float toneL = 0.0f;
			float toneR = 0.0f;
			// THE NOTES' STRINGS' CONTROLS, per sample (a pole, not a block ramp: how a host cuts blocks changes nothing), and
			// the ring's orbit eased across the window as the gate is.
			stringBlend_ += stringCoeff * (blendTarget - stringBlend_);
			if (std::abs(blendTarget - stringBlend_) < 1e-7f) {
				stringBlend_ = blendTarget;
			}
			stringExcite_ += stringCoeff * (exciteTarget - stringExcite_);
			if (std::abs(exciteTarget - stringExcite_) < 1e-7f) {
				stringExcite_ = exciteTarget;
			}
			proximity_ = proxPrev_ + (proxTarget_ - proxPrev_) * t;
			const float stringBlend = stringBlend_;
			const float blendScan = std::cos(1.5707963f * stringBlend);
			const float blendString = std::sin(1.5707963f * stringBlend);
			const float dryMono = 0.5f * (dryL + dryR);
			// THE BRIDGE (v18, kBridgeMassBottom): its next displacement from every string's pull on it now, and every string on it
			// steps this sample with its end there (the notes' in read, the drone's below). A string joins seated where the bridge
			// is. RIGID (both Sympathies 0) it falls back to rest, ignoring the pulls, and then lets the strings go.
			// TWO GROUPS ON ONE BRIDGE (v21, a new sympathy control for drones only): the bridge's mass is the lighter of
			// the two the knobs name (Sympathy the notes', Drone Sympathy the drone's), and each group hangs on it through an
			// ideal LEVER of ratio c = sqrt(mass / its own) — its end at c w, its pull on the bridge c x its force — so each group
			// sees exactly its own knob's mass and the exchange stays passive (bridgeCouplingFor). At equal knobs c is 1 to the
			// bit, the v20 bridge; at 0 a group is off the bridge (c 0, its ends at rest).
			// ONLY WHEN A KNOB MOVES (a pow and a sqrt each, else every sample).
			if (sympathy != bridgeSympathyAt_ || droneSympathy != bridgeDroneSympathyAt_) {
				bridgeSympathyAt_ = sympathy;
				bridgeDroneSympathyAt_ = droneSympathy;
				// THE SAME MASS ON BOTH LAWS (the defaults: 150 on each, which the two float knobs reach only to a part in 1e7) IS ONE
				// MASS: the drone law's, with both levers 1, so the default bridge is v20's to the bit.
				double massNotes = bridgeMassNotes(sympathy);
				const double massDrone = bridgeMass(droneSympathy);
				if (massNotes > 0.0 && massDrone > 0.0 && std::abs(massNotes / massDrone - 1.0) < 1e-6) {
					massNotes = massDrone;
				}
				bridgeMass_ = (massNotes > 0.0 && massDrone > 0.0) ? std::min(massNotes, massDrone) : std::max(massNotes, massDrone);
				bridgeCouplingNotes_ = bridgeCouplingFor(bridgeMass_, massNotes);
				bridgeCouplingDrone_ = bridgeCouplingFor(bridgeMass_, massDrone);
			}
			const double mass = bridgeMass_;
			const double cNotes = bridgeCouplingNotes_;
			const double cDrone = bridgeCouplingDrone_;
			double bridgeNext = 0.0;
			{
				const auto seat = [&](NodeString& x, double c) {
					const bool on = bridgeYields_ && c > 0.0;
					x.setYield(on, static_cast<float>(c * bridgeW_), static_cast<float>(c * bridgeWPrev_));
					return on ? c * static_cast<double>(x.bridgePull()) : 0.0;
				};
				double pull = 0.0;
				for (DroneString& d : strings_) {
					if (d.active) {
						pull += seat(d.node, cDrone);
					}
				}
				for (const Head& h : heads_) {
					NodeString& v = voiceStrings_[static_cast<size_t>(&h - heads_.data())];
					if (h.live && stringBlend != 0.0f && notesOnBridge_) {
						pull += seat(v, cNotes);
					}
					else {
						v.setYield(false);
					}
				}
				bridgeNext = mass > 0.0 ? 2.0 * bridgeW_ - bridgeWPrev_ + pull / mass - bridgeSpring * bridgeW_ - kBridgeDamping * (bridgeW_ - bridgeWPrev_)
				                        : bridgeW_ * bridgeRest;
				if (std::abs(bridgeNext) < 1e-15 && std::abs(bridgeW_) < 1e-15) {
					bridgeNext = 0.0; // EXACT ZERO, as a string's
				}
			}
			lastVoiceStringL_ = 0.0f;
			// THE NOTE HEADS' GAIN: the motion gate at Sustain 0, a steady 1 at Sustain 1.
			const float toneGain = gate_ + (1.0f - gate_) * sustain;
			lastToneGain_ = toneGain;
			// EACH HEAD'S OWN SCAN BURST falls (v19: a burst is the head's, set at the ring pluck that is its own: notePluck, deliverCyclePluck).
			for (Head& h : heads_) {
				h.drivePluck *= drivePluckFall;
			}
			// A RELEASED HEAD (v14) is summed apart, at its own gain (under its hold, startRelease), so a held head's sum
			// times the gate is v13's to the bit.
			float relL = 0.0f;
			float relR = 0.0f;
			auto read = [&](Head& h) {
				stepHead(h);
				const size_t slot = static_cast<size_t>(&h - heads_.data());
				headOutL_[slot] = 0.0f;
				headScanGain_[slot] = 0.0f;
				headMotion_[slot] = 0.0f;
				if (!h.live) {
					return;
				}
				const int mip = WeaveString::mipForFrequency(h.inc * sr_);
				const double at = h.phase + f.travel;
				const float scanL = ring_.sample(f, at, mip);
				const float scanR = ring_.sample(f, at + static_cast<double>(kWidthCycles) * width, mip);
				// THE RING'S MOTION at the head (v15): the scan less the scanned rest shape, through the same tables, so 0 at rest.
				const float motionL = ring_.sampleMotion(f, at, mip);
				const float motionR = ring_.sampleMotion(f, at + static_cast<double>(kWidthCycles) * width, mip);
				const float raw = 0.5f * std::abs(motionL + motionR) * kMotionDriveGain;
				h.driveEnv = std::max(raw, h.driveEnv * driveCapFall);
				const float motionDrive = motionDriveOn_ ? kMotionDriveGain * (driveCapOn_ && h.driveEnv > kDriveCap ? kDriveCap / h.driveEnv : 1.0f) : 0.0f;
				headMotion_[slot] = h.released ? 0.0f : 0.5f * (motionL + motionR) * motionDrive;
				const float scanGain = h.released ? std::min({ h.gain * toneGain, h.hold, h.scanCap }) : 0.0f;
				h.scanCap = scanGain;
				headScanGain_[slot] = h.released ? scanGain : h.gain * toneGain;
				if (stringBlend == 0.0f) {
					// STRING 0 IS v9'S TONE, BIT FOR BIT: the strings are not run.
					if (h.released) {
						relL += scanGain * scanL;
						relR += scanGain * scanR;
						headOutL_[slot] = scanGain * scanL;
						return;
					}
					toneL += h.gain * scanL;
					toneR += h.gain * scanR;
					headOutL_[slot] = h.gain * scanL * toneGain;
					return;
				}
				// THE HEAD'S OWN STRING: at the head's pitch, ringing for the strings' T60, its bridge brought closer by the
				// ring's orbit, played by the ring's motion (v15) with a burst of its scanned voice at each ring pluck, and by the dry
				// input (String Excite), heard at two pickups —
				// until its key is up: then it is no longer driven and rings on its own loss (v14).
				NodeString& v = voiceStrings_[slot];
				v.tune(h.inc * sr_, stringT60);
				v.setBridge(settings_.jawari, clamp01(settings_.bounce), orbitOnBridge_ ? proximity_ : 0.0f);
				v.setFixed(clamp01(settings_.bridgeFixed), kFixedNoteRef);
				v.setDrag(dragOn_ ? kNoteDragLevel : 0.0f);
				// x ITS KEY'S VELOCITY (v13: 1 with no note-on velocity, so audio alone is as before).
				const float force = h.released ? 0.0f
				                               : ((1.0f - stringExcite_) * 0.5f * ((motionL + motionR) * motionDrive + (scanL + scanR) * kDrivePluckGain * h.drivePluck)
				                                  + stringExcite_ * kStringInputGain * dryMono)
				                                     * keyVelocity_[static_cast<size_t>(std::clamp(h.key, 0, 127))];
				float sL = 0.0f;
				float sR = 0.0f;
				v.setBridgeNext(static_cast<float>(cNotes * bridgeNext)); // its end on its lever (v21)
				v.process(force, width, sL, sR);
				sL *= kVoiceStringLevel;
				sR *= kVoiceStringLevel;
				lastVoiceStringL_ += h.gain * sL;
				if (h.released) {
					// ITS STRING AT THE GAIN IT WAS HEARD UNDER AT THE NOTE-OFF (v15: v14 let it go "ungated", a jump of 1 / the gate —
					// re-attacking on release), and its own release ramp; ITS SCANNED VOICE UNDER ITS HOLD.
					const float l = scanGain * blendScan * scanL + h.gain * h.stringGain * blendString * sL;
					relL += l;
					relR += scanGain * blendScan * scanR + h.gain * h.stringGain * blendString * sR;
					headOutL_[slot] = l;
					return;
				}
				// AN EQUAL-POWER BLEND: the string and the scanned voice are far from in phase, so a linear one dipped ~4 dB at
				// the middle (MEASURED at the default).
				toneL += h.gain * (blendScan * scanL + blendString * sL);
				toneR += h.gain * (blendScan * scanR + blendString * sR);
				headOutL_[slot] = h.gain * (blendScan * scanL + blendString * sL) * toneGain;
			};
			// EVERY HEAD IS STEPPED AND READ IN EITHER MODE: a head released by a Voicing change or a steal
			// finishes its fade whatever the mode now is.
			glide();
			for (Head& h : heads_) {
				read(h);
			}
			toneL = toneL * toneGain + relL;
			toneR = toneR * toneGain + relR;
			// AND ON THE NOTES (v11): a body colours what is played on its instrument, not only its drone.
			if (bodyOn_) {
				bodyStep(toneL, toneR, bodyTone_, bodyToneMakeup_);
			}
			// THE DRONE'S STRINGS: each follows the input — through its own harmonic ear (TUNED), the whole input (INTEGRATED),
			// or a blend of the two by Drone Excite, into its node string; its two voices sound at that string's own level
			// (v12: the scanned voice's gain is kScanMakeup x the string's, never a follower of the input) x its level (x its
			// retune fade), and nothing else — no motion gate, no Sustain, and neither Level nor
			// Blend: the drone joins the wet after them. Under kDroneFloor it is exactly silent, so a string ringing down ends.
			float droneL = 0.0f;
			lastDroneVoiceL_ = lastDroneScanL_ = 0.0f;
			float droneR = 0.0f;
			droneInputGain_ += inputCoeff * (inputTarget - droneInputGain_);
			if (std::abs(inputTarget - droneInputGain_) < 1e-7f) {
				droneInputGain_ = inputTarget;
			}
			const float heard = droneInputGain_ * (0.5f * (dryL + dryR));
			++sampleClock_;
			// THE CYCLE, ON THE GRID (cyclePluck names the string). While the transport PLAYS with a tempo and a beats
			// position, a pluck lands at the first sample at or past each multiple of the division in the song's beats —
			// the block's position plus this sample's share of a beat — so it is sample-accurate and cannot drift. Stopped
			// (or with no position), it runs FREE at the same division at the host's tempo (one second a beat if none,
			// the delay's fallback), counted from when it started — and the moment it is turned on, it plucks.
			// A LED PLUCK WAITING FOR ITS GRID SAMPLE: its ring pluck, follower kick and log land on this one (cyclePluck).
			if (pendingPluck_.countdown > 0 && --pendingPluck_.countdown == 0) {
				deliverCyclePluck();
			}
			if (cycleOn) {
				// THE LEAD (pluckLead()): how far ahead of its grid sample a pluck starts so that it PEAKS on it (v11, by
				// design: v7-v10 started a comb bump on the grid, peaking half a period late — 7.2 ms at C2). 0 for a node string.
				const int lead = pluckLead();
				if (cycleLocked) {
					// THE NEXT GRID INDEX DUE is held (cycleNextIndex_), not re-derived from a position that moves with the lead
					// (the lead changes with each pluck's string): due when the song, `lead` samples ahead, reaches it.
					const double perSample = 1.0 / (sr_ * secondsPerBeat);
					const double beatNow = clock_.songPositionBeats + static_cast<double>(i) * perSample;
					const double qNow = beatNow / cyclePeriod;
					const double qLead = (beatNow + static_cast<double>(lead) * perSample) / cyclePeriod;
					// (RE)LOCK: at the start, or when the song jumped (a loop back, a locate, anything that skips a grid point).
					// (A pluck's grid point is at most kCycleRelockSeconds / 2 behind the song once it is led, so a song
					// further back than that behind the last grid plucked has jumped.)
					const double maxLead = 0.5 * kCycleRelockSeconds / (secondsPerBeat * cyclePeriod);
					if (!cycleHasIndex_ || qNow < cycleNextIndex_ - 1.0 - maxLead - 1e-9 || qLead >= cycleNextIndex_ + 1.0) {
						// A LED PLUCK FOR A GRID POINT THE SONG NEVER REACHED (the host looped or located inside its lead) is
						// withdrawn: neither it nor its ring pluck happens.
						if (cycleHasIndex_ && pendingPluck_.countdown > 0) {
							strings_[static_cast<size_t>(pendingPluck_.string)].node.endStrike();
							pendingPluck_.countdown = -1;
						}
						const double indexNow = std::floor(qNow + 1e-9);
						if (qNow - indexNow < perSample / cyclePeriod) {
							// a grid point AT this sample: here, unled (it cannot be led) — unless a led pluck already landed on this very
							// sample (a host loop back to a grid point, from a loop end that was one: the lead saw the end coming)
							if (lastPluckAt_ != sampleClock_) {
								cyclePluck(0);
							}
							cycleNextIndex_ = indexNow + 1.0;
						}
						else {
							cycleNextIndex_ = indexNow + 1.0;
						}
						cycleHasIndex_ = true;
					}
					if (qLead + 1e-9 >= cycleNextIndex_) {
						const double ahead = std::ceil((cycleNextIndex_ * cyclePeriod - beatNow) / perSample - 1e-6);
						cyclePluck(static_cast<int>(std::clamp(ahead, 0.0, static_cast<double>(lead))));
						cycleNextIndex_ += 1.0;
					}
					cycleCountdown_ = cycleInterval; // stopping carries on a whole division later
				}
				else {
					cycleHasIndex_ = false;
					if (cycleCountdown_ <= lead) {
						cyclePluck(std::max(0, cycleCountdown_));
						cycleCountdown_ += cycleInterval;
					}
					--cycleCountdown_;
				}
			}
			else {
				cycleCountdown_ = 0;
				cycleHasIndex_ = false;
			}

			for (size_t k = 0; k < kMaxDroneStrings; ++k) {
				DroneString& d = strings_[k];
				if (!d.active) {
					continue;
				}
				// ITS EAR HEARS THE INPUT ALONE (v18): what the other strings give it comes through its bridge end.
				const float ear = heard;
				const float y = d.ear.process(ear);
				d.level += (d.levelTarget - d.level) * levelCoeff;
				// THE STRING'S FEED: Excite's tuned <-> integrated is now the input fed through the string's
				// harmonic ear (its own series) or fed straight. DRONE ATTACK IS ITS SWELL: while anything is fed (a presence
				// envelope, 50 ms, above kSwellFloor) a gain rises toward 1 with the string's attack time constant, and with
				// nothing fed it falls back over kSwellFallSeconds. A gain that moves only at onsets and silences — exactly 1
				// in steady state — so it adds no ripple, and it is never above 1, so it boosts nothing.
				const float feed = (1.0f - excite) * y + excite * ear;
				d.feed = feed;
				d.swellFast = std::max(std::abs(feed), d.swellFast * swellRelease);
				if (d.swellFast < 1e-20f) {
					d.swellFast = 0.0f;
				}
				const float target = d.swellFast > kSwellFloor ? 1.0f : 0.0f;
				d.swellSlow += (target - d.swellSlow) * (target > d.swellSlow ? d.swellCoeff : swellFall);
				if (d.swellSlow > 0.99999f) {
					d.swellSlow = 1.0f;
				}
				else if (d.swellSlow < 1e-6f && target == 0.0f) {
					d.swellSlow = 0.0f;
				}
				// ITS FEED BOUND (v15): past kDroneFeedBound of heard level, its feed falls away (a strike is not a feed).
				const float heardNow = d.node.heardEnv();
				const float boundTarget = (feedBoundOn_ && heardNow > kDroneFeedBound)
				                              ? std::max(0.0f, 1.0f - kDroneFeedSlope * (heardNow / kDroneFeedBound - 1.0f))
				                              : 1.0f;
				d.feedGain += (boundTarget - d.feedGain) * feedBoundCoeff;
				const float drive = feed * d.swellSlow * d.feedGain;
				// ITS NODE STRING (v11), driven by the feed (NodeString scales a force by its own loss), its point bridge as the notes' —
				// Jawari, Bounce — brought closer by the ring's orbit at half the notes' reach (kDroneOrbitReach), heard at its
				// pickup. GEOMETRIC (v17): the bridge at a fixed height (kFixedDroneRef), not one that follows the string's swing —
				// MEASURED the better drone in the point prototype (monotonic in Jawari, more buzz, in tune), and it needs no bloom
				// and no makeup. A Cycle pluck is a strike at its drive point, started by cyclePluck.
				d.node.setBridge(settings_.droneJawari, clamp01(settings_.droneBounce), kDroneOrbitReach * (orbitOnBridge_ ? proximity_ : 0.0f)); // v20: its own
				// v21: the Drone Thread (its default, a float, read as the exact 3% the notes use: 0.03f is not 0.03, and lround(0.03 x 50)
				// sits on the half).
				d.node.setPointPos(settings_.droneThread == static_cast<float>(kDroneThreadDefault) ? kDroneThreadDefault
				                                                                                    : static_cast<double>(settings_.droneThread));
				d.node.setFixed(1.0f, kFixedDroneRef, kFixedDroneSlope);
				float yc = 0.0f;
				float ycR = 0.0f;
				d.node.setBridgeNext(static_cast<float>(cDrone * bridgeNext)); // its end on its lever (v21)
				d.node.process(kNodeMakeup * drive, 0.0f, yc, ycR);
				d.voiceOut = yc;
				d.voiceEnv = std::max(std::abs(yc), d.voiceEnv * envRelease);
				if (d.voiceEnv < 1e-20f) {
					d.voiceEnv = 0.0f;
				}
				d.voiceMs += d.levelOwn * (yc * yc - d.voiceMs);
				if (d.voiceMs < 1e-30f) {
					d.voiceMs = 0.0f; // exact zero: a rung-out string is silent in both its voices
				}
				if (d.retuning) {
					d.fade = std::max(0.0f, d.fade - fadeStep);
					if (d.fade <= 0.0f) {
						// FADED OUT: it goes, or it retunes from silence (a fresh ear, voice and follower) and fades in.
						if (d.leaving) {
							resetString(k);
							continue;
						}
						d.interval = d.pending;
						d.pitch = d.pendingPitch;
						d.detune = d.pendingDetune;
						d.root = d.pendingRoot;
						d.retuning = false;
						d.voiceMs = d.swellFast = d.swellSlow = d.voiceOut = d.voiceEnv = 0.0f;
						d.ear.clear();
						d.node.clear();
						configureString(d, offset);
					}
				}
				else {
					d.fade = std::min(1.0f, d.fade + fadeStep);
				}
				// THE SCANNED VOICE'S GAIN: its string's own level (kScanMakeup), never past 1, so it rings and decays with it.
				// v14 : THE LARGER OF its string's own level and the MAIN STRING'S MOTION (the motion gate), the motion
				// rising no faster than its Drone Attack (its swell's time constant: a slow swell, never a jump) and falling over its
				// Decay — so dragging the puck, which bows the ring, sounds the drone again with no input and no Cycle. Only while the
				// string is PLAYED (played_: the input, a pluck, the puck): the Bow alone moves it for ever.
				const float moving = played_ ? gate_ : 0.0f;
				d.motion += (moving - d.motion) * (moving > d.motion ? d.swellCoeff : d.motionFall);
				if (d.motion < 1e-9f) {
					d.motion = 0.0f;
				}
				// FOLLOW OFF (v21): its own string's level alone — the main string's motion no longer swells it.
				const float own = settings_.droneFollow != 0 ? std::max(kScanMakeup * std::sqrt(d.voiceMs), d.motion) : kScanMakeup * std::sqrt(d.voiceMs);
				const float ring = own * d.level * d.fade;
				const float g = droneOut * ring;
				d.gain = (g >= kDroneFloor) ? g : 0.0f;
				d.phase = wrap01(d.phase + d.inc);
				// WHAT IT SOUNDS AT, BOTH VOICES — under kDroneFloor exactly silent, so a string ringing down ends.
				const float sounding = droneOut * d.level * d.fade
				                       * std::max((1.0f - voice) * std::min(1.0f, d.voiceEnv), voice * std::min(1.0f, own));
				d.sounding = (sounding >= kDroneFloor) ? sounding : 0.0f;
				d.outL = 0.0f;
				if (d.sounding > 0.0f) {
					const float lf = droneOut * d.level * d.fade;
					const float comb = (1.0f - voice) * lf * d.voiceOut;
					float scanL = 0.0f;
					float scanR = 0.0f;
					if (voice > 0.0f && d.gain > 0.0f) {
						const int mip = WeaveString::mipForFrequency(d.inc * sr_);
						const double at = d.phase + f.travel;
						scanL = voice * d.gain * ring_.sample(f, at, mip);
						scanR = voice * d.gain * ring_.sample(f, at + static_cast<double>(kWidthCycles) * width, mip);
					}
					d.outL = d.panL * (comb + scanL);
					droneL += d.outL;
					lastDroneVoiceL_ += d.panL * comb;
					lastDroneScanL_ += d.panL * scanL;
					droneR += d.panR * (comb + scanR);
				}
			}
			// THE BRIDGE MOVES ON (v18), and the strings ride it until a rigid one is back at rest.
			bridgeWPrev_ = bridgeW_;
			bridgeW_ = bridgeNext;
			bridgeYields_ = mass > 0.0 || bridgeW_ != 0.0 || bridgeWPrev_ != 0.0;
			// THE GOURD BODY (the twin gourds in it) on what the strings sound (Body 0 is skipped, bit for bit).
			if (bodyOn_) {
				bodyStep(droneL, droneR, bodyDrone_, bodyMakeup_);
			}


			// THE FILTER: the input through the string's shape, the window fading from the last tick's
			// kernel to this one's (smoothstep, as the string's own tables fade).
			// The history is written whether or not the filter is heard, so it is whole when Blend moves.
			// (unless the FIR's look-ahead, below, has written this sample's already: the same value)
			if (k >= historyAhead) {
				const size_t hp = static_cast<size_t>(historyPos_);
				history_[0][hp] = history_[0][hp + kHistory] = dryL;
				history_[1][hp] = history_[1][hp + kHistory] = dryR;
				historyAhead = k + 1;
			}
			float filt[2] = { 0.0f, 0.0f };
			// BLEND AT 0 IS UNHEARD, so the FIR is skipped; the moment Blend leaves 0, kernels skipped at the
			// ticks meanwhile are seated from the string as it is now.
			if (!blendIdle()) {
				if (kernelStale_) {
					seatKernels();
					firEnd = k; // nothing computed ahead survives new kernels (none was: a stale kernel is seated first)
				}
				// THE SUMS, kFirAhead samples at a time (firAhead): the kernels move only at the tick that ends this window, or
				// at the seat above.
				if (referenceForTest_) {
					// THE REFERENCE (setReferenceForTest): this sample alone, the scalar sum the loop made before firAhead.
					firAhead(left + i, right + i, 1);
					firBegin = k;
					firEnd = k;
				}
				else if (k >= firEnd) {
					const int count = std::min(m - k, kFirAhead);
					firAhead(left + i, right + i, count);
					firBegin = k;
					firEnd = k + count;
					historyAhead = std::max(historyAhead, firEnd);
				}
				const float tk = smoothstep(t);
				const size_t at = static_cast<size_t>(k - firBegin);
				for (int ch = 0; ch < 2; ++ch) {
					const float a = firFrom_[static_cast<size_t>(ch)][at];
					const float b = firTo_[static_cast<size_t>(ch)][at];
					filt[ch] = a + (b - a) * tk;
				}
			}
			historyPos_ = (historyPos_ + 1) & (kHistory - 1);

			// THE WET: Level x the tone and the filter blended, and the drone beside them at its own output level.
			const float toneWetL = levelGain * ((1.0f - blend) * toneL + blend * filt[0]);
			const float toneWetR = levelGain * ((1.0f - blend) * toneR + blend * filt[1]);
			const float wetL = toneWetL + droneL;
			const float wetR = toneWetR + droneR;
			if (tap_ != nullptr) {
				const auto at = static_cast<size_t>(i);
				tap_->dry[0][at] = dryL;
				tap_->dry[1][at] = dryR;
				tap_->tone[0][at] = toneWetL;
				tap_->tone[1][at] = toneWetR;
				tap_->drone[0][at] = droneL;
				tap_->drone[1][at] = droneR;
				tap_->mix[at] = mix;
				tap_->enable[at] = enable;
			}
			const float mixedL = (1.0f - mix) * dryL + mix * wetL;
			const float mixedR = (1.0f - mix) * dryR + mix * wetR;
			const float outL = finiteOr0(dryL + enable * (mixedL - dryL));
			const float outR = finiteOr0(dryR + enable * (mixedR - dryR));
			left[i] = outL;
			right[i] = outR;

			// REST: every condition, every sample; the count restarts the moment one fails.
			// The tone contributes nothing when the drone is at 0 and the note heads add nothing: none sounds, or
			// Sustain is 0 and the string is still.
			const bool toneSilent = !anyDroneSounding() && (!anyNoteHeadLive() || (sustain == 0.0f && ringStill()));
			const bool idle = !cycleOn && dryL == 0.0f && dryR == 0.0f && settings_.bow <= 0.0f && toneSilent && morph_.settled()
			                  && std::abs(outL) < kRestLevel && std::abs(outR) < kRestLevel;
			idleSamples_ = idle ? idleSamples_ + 1 : 0;
		}

		if (m == untilTick) {
			// THE TICK: the excitation the string runs its next two sub-steps under, sampled here.
			const float level = std::max(follower_.level(automod::Channel::Left), follower_.level(automod::Channel::Right));
			ring_.setExcitation({ kBowGain * settings_.bow + settings_.drive * level * kDriveGain });
			if (settings_.bow > 0.0f) {
				excitedThisWindow_ = true;
			}
			// THE CARD'S INPUT METER: the level Drive hears this window.
			meterLevel_ = level;
			// MORPH MOTION BOWS THE STRING (the firmware's and the design's rule), so it is excitation: it holds the
			// gate open like the input or the Bow. The string itself turns it into morph-bow force.
			if (std::abs(morph_.value() - lastTickMorph_) > kMorphMotionEpsilon) {
				excitedThisWindow_ = playedThisWindow_ = true;
			}
			lastTickMorph_ = morph_.value();
			ring_.setCrossfade(morph_.value());
			ring_.advance(m);
			onTick();
		}
		else {
			ring_.advance(m);
		}
		done += m;

		if (!resting_ && idleSamples_ >= restSamples) {
			// PARK: the ear and the filter's memory cleared; output was already under kRestLevel and from here
			// is exactly zero until something wakes the device. A STILL string is seated at rest; a string that
			// is still moving (it may park only because no head is reading it) is FROZEN, not reset, and
			// resumes where it was when a head or the input wakes it.
			resting_ = true;
			restPublishCount_ = 0;
			parkedMorph_ = morph_.value();
			if (ringStill()) {
				ring_.reset();
				ring_.pluck(0.0f);
				gate_ = gatePrev_ = gateTarget_ = gateEnergy_ = 0.0f;
				gateLatched_ = true; // shut until something excites the string again
			}
			follower_.reset();
			for (DroneString& d : strings_) { // under the floor already: the drone was silent, or the device could not park
				d.motion = d.voiceMs = d.gain = d.sounding = d.swellFast = d.swellSlow = d.voiceOut = d.voiceEnv = 0.0f;
				d.feedGain = 1.0f;
				d.ear.clear();
				d.node.clear();
			}
			for (NodeString& v : voiceStrings_) { // the notes' too (v16: the drone's bridge reaches them), under the floor as well
				v.clear();
			}
			bridgeW_ = bridgeWPrev_ = 0.0;
			bridgeYields_ = false;
			clearBody();
			for (auto& h : history_) {
				h.fill(0.0f);
			}
			excitedThisWindow_ = playedThisWindow_ = played_ = false;
		}
	}
}

bool ResonateDsp::holdsTail() const {
	if (!settings_.enabled || !cycleRunning() || (settings_.droneCycleTransport != 0 && !clock_.playing)) {
		return false;
	}
	for (const DroneString& d : strings_) {
		if (d.active && !d.leaving) {
			return true;
		}
	}
	return false;
}

} // namespace feathers::fx::resonate
