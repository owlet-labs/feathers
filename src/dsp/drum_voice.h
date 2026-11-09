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

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "dsp/ks_string.h"

// ONE DRUM HIT: a small drum synthesiser of one of six KINDS, struck
// once and left to ring out to EXACT silence. Host rate, mono, one per OscVoice; the voice pans it.
//
// EVERY KIND IS AN EXCITER, A BODY AND A DECAY:
//   Kick     a sine whose pitch falls exponentially from Hz·2^Sweep to Hz, self-phase-modulated by Tone, plus a click
//            (a noise burst at Snap's level), through Drive's soft clip
//   Membrane a DAMPED MODAL BANK: kMembraneModes rotators at the circular membrane's ratios, morphed by Metal toward a
//            free bar's; each mode its own T60 (higher modes shorter), its strike amplitude set by Tone (strike position);
//            Sweep bends the whole bank down from above (a hard-hit head's tension); Snap a mallet click; Noise a band
//   Snare    a Membrane head of kSnareHeadModes modes plus the WIRES: noise through a band-pass at Tone, their own decay;
//            Noise sets the wires against the head
//   Metal    six band-limited squares at inharmonic ratios spread by Metal, plus Noise's share of white noise, through a
//            band-pass at Tone and a one-pole high-pass; Snap lifts the first milliseconds
//   Clap     noise through a band-pass at Tone, struck as Count bursts across Spread (their spacing jittered by the hit's
//            seed) then a tail at Decay; Noise sets the tail's level
//   String   the voice's Karplus-Strong string (dsp/ks_string.h), plucked with noise: Tone its brightness and pick, Snap
//            the pluck's hardness, Metal its stretch, Sweep a bend down from above
//
// THE SEVEN FIELDS (proposal §3.1) mean one physical thing to every kind, so a place keeps its meaning across kinds; a
// kind that does not read a field ignores it (and the UI does not draw its scaler: `kindReads`).
//
// EXACT SILENCE (proposal §4.4; CLAUDE.md, a host-facing "I'm done" is decided by the output): every envelope and every
// mode is snapped to 0 below kSilenceFloor (−160 dB, the Additive bank's and the string's floor). Once nothing is left the
// hit zeroes every filter state and is IDLE: it renders exact zeros and costs nothing until the next strike.
//
// DETERMINISM (proposal §7): every noise sample, every clap jitter and the hit's variation come from the seed the caller
// derives (from the song position on a playing transport), through ks::PluckNoise. Nothing is drawn from anywhere else.
//
// THREADING: everything here is the audio thread's and never allocates; the String kind borrows a string the caller
// prepared (at activate).

namespace feathers::drum {

enum class Kind : uint8_t { Kick, Membrane, Snare, Metal, Clap, String, kCount };
inline constexpr size_t kKindCount = static_cast<size_t>(Kind::kCount);
/// The labels a Choice stores (the schema ledger stores a Choice by label, so this order may change; a label may not
/// without a ChoiceAlias).
inline constexpr std::array<const char* const, kKindCount> kKindNames { "Kick", "Membrane", "Snare", "Metal", "Clap",
	                                                                    "String" };

/// THE SEVEN DRUM FIELDS (proposal §3.1), each in [0, 1]: the terrain's, or a scaler's override of it.
struct Fields {
	float pitch{0.5f};  ///< a fine offset around Tune: 0.5 is none
	float length{0.5f}; ///< a multiplier on Decay, 4^(2·length − 1): 0.5 is ×1
	float sweep{0.0f};  ///< the depth of the pitch drop, of the kind's own range
	float tone{0.5f};   ///< brightness: strike position, filter centre, the click's colour
	float noise{0.0f};  ///< the noise share
	float snap{0.5f};   ///< the transient's hardness
	float metal{0.0f};  ///< inharmonicity: the ratio spread
	friend bool operator==(const Fields&, const Fields&) = default;
};

/// Which of the seven fields a kind reads, in Fields' order (pitch, length, sweep, tone, noise, snap, metal). The ONE
/// table the UI's honesty rule and the engine agree on: a field a kind does not read is not drawn on it.
struct FieldUse {
	bool pitch, length, sweep, tone, noise, snap, metal;
};
FieldUse kindReads(Kind k);

/// Which of the per-pad absolutes a kind reads (the rest are not drawn on it).
struct ControlUse {
	bool sweepTime, drive, count, spread;
	/// The Snare's own (multiple snare envelopes are essential, and snares sometimes have moving bells/band-passes):
	/// the wires' and the click's decays, the body's and the wires' shapes, the wires' filter sweep, and the moving bell.
	bool snareEnvelopes, bell;
	/// The Kick's shape (proposal §12 items 1-3): Hold and the fishtail dip, the click sweep, the start phase.
	bool kickShape;
	bool tension;   ///< the Sweep field is Tension here, and Velocity Tension is read (Kick, Membrane, Snare)
	bool velNoise;  ///< Velocity Noise is read (the kind reads Noise)
	bool clapFilter;
	bool wireContour; ///< a Snare's wire Contour and Buzz
	bool width;       ///< Width (Snare, Clap)
};
ControlUse kindControls(Kind k);

/// THE PER-PAD CONTROLS a hit needs (proposal §7's absolutes), resolved by the caller.
struct Controls {
	double decaySeconds{0.5};  ///< the body's T60 before Length
	double sweepSeconds{0.06}; ///< the sweep's time: the drop is 95% done by then (τ = sweepSeconds / 3)
	double drive{0.0};         ///< 0..1, how hard every kind is pushed into its drive curve
	int count{3};              ///< 1..8, the Clap's bursts
	double spreadSeconds{0.03};///< the Clap's bursts span this
	/// THE THREE ZONE COORDINATES (drum proposal §14): one control each whose travel passes through named
	/// zones (wiresAt, bellsAt, velocityAt), a lap of the named set every 1.0. Velocity at 2/3 is today's Velocity 0.5 and the
	/// tension law in full; Wires 0 is Tight, Bells 0 is Off.
	double wires{0.0};
	double bells{0.0};
	double velocityZone{2.0 / 3.0};
	double pitchRangeSemitones{3.0}; ///< the Pitch field's reach either side (proposal §3.1: 3 in Kit, 0.5 in Pitched)
	// THE SNARE'S ENVELOPES AND ITS BELL (drum proposal §12), read only on a Snare.
	double wireDecaySeconds{0.18}; ///< the wires' T60, Length applied as to the body's
	double clickDecaySeconds{0.006};///< the stick click's T60
	double bellStartHz{2500.0};    ///< the moving bell: a resonant band-pass whose centre glides from here
	double bellEndHz{700.0};       ///< to here,
	double bellSeconds{0.04};      ///< 95% of the way by this time,
	// THE KICK'S SHAPE (drum proposal §12 items 1-3), read only on a Kick.
	double kickHoldSeconds{0.0};       ///< the body stays at full this long before it decays
	double kickDip{0.0};               ///< the fishtail: how deep the level dips (0..1)
	double kickDipSeconds{0.014};      ///< where the dip is deepest; it spans half that either side
	double clickSweepOctaves{0.0};     ///< a second, very short pitch drop on top of Sweep
	double clickSweepSeconds{0.003};   ///< its time (95% landed)
	double startPhaseDegrees{0.0};     ///< where the sine starts, 0..90
	int driveCurve{0};                 ///< the drive's curve on every kind: 0 Tanh, 1 Tube, 2 Soft Clip
	double driveTone{0.5};             ///< a tilt before the drive: 0 feeds it the lows, 1 the highs, 0.5 flat
	double clapQ{1.6};                 ///< the Clap's band-pass Q
	double clapHighPassHz{20.0};       ///< the Clap's high-pass corner (20 Hz: nothing taken)
	double bell2Hz{500.0};
	double bell2Seconds{0.03};         ///< its level's T60
	double velocityNoise{0.0};         ///< −1..1: how far velocity moves the noise share, as velocityTone moves Tone
	double width{0.0};                 ///< 0..1: a Snare's wires or a Clap's band decorrelated between the ears
	friend bool operator==(const Controls&, const Controls&) = default;
};

/// **THE ZONE CONTROLS** (drum proposal §14 lever 2, approved by, names and stops a first cut). One coordinate
/// each, read as a ZoneCoordCell reads it: its fractional part runs once through the named zones (a lap per 1.0, so the line
/// is unbounded and wraps), each zone an ANCHOR of consolidated params, and between two zones every continuous param moves
/// linearly from one anchor to the next; a choice (the Contour, the bell's Link) switches at the midpoint.
inline constexpr std::array<const char* const, 4> kWiresZoneNames { "Tight", "Loose", "Buzzy", "Gated" };
inline constexpr std::array<const char* const, 5> kBellsZoneNames { "Off", "Body", "Shell", "Ring", "Knock" };
inline constexpr std::array<const char* const, 3> kVelocityZoneNames { "Level", "Tone", "Tension" }; // cumulative: Tension keeps Tone (a cell fits ~8 letters)
struct WiresMix {
	int contour{0};          ///< 0 Exp, 1 Linear, 2 Mod
	double wireShape{0.0};
	double bodyShape{0.0};
	double buzz{0.0};
	double threshold{0.5};
	friend bool operator==(const WiresMix&, const WiresMix&) = default;
};
struct BellsMix {
	double level1{0.0};
	double level2{0.0};
	double q1Scale{1.0};     ///< on the Q the place's Snap gives (bellQFor)
	double q2Scale{1.0};
	bool linked{true};       ///< the second bell at Metal's ratio × the first (bell2RatioFor), else Free at its Hz
	friend bool operator==(const BellsMix&, const BellsMix&) = default;
};
struct VelocityMix {
	double tone{0.5};        ///< how far velocity moves Tone and Snap, x + tone·(v − 0.5)
	double tension{1.0};     ///< how much of the tension law (glide depth ∝ velocity²) applies
	friend bool operator==(const VelocityMix&, const VelocityMix&) = default;
};
WiresMix wiresAt(double coordinate);
BellsMix bellsAt(double coordinate);
VelocityMix velocityAt(double coordinate);
/// **WHAT YIELDS TO THE TERRAIN** (drum proposal §14 lever 1): a Snare's wire sweep is its Sweep field (its Tension) × one
/// octave; the bells' Q is the place's Snap (2 + 10·Snap for the first, 1 + 6·Snap for the second, each × its zone's scale);
/// a linked second bell's ratio is the place's Metal, 1.3 + 1.2·Metal.
double wireSweepFor(float sweepField);
double bellQFor(float snap, double scale);
double bell2QFor(float snap, double scale);
double bell2RatioFor(float metal);

/// ONE PAD AS THE SYNTH RESOLVES IT EACH CHUNK (proposal §7): its kind, its controls, and what the voice applies around the
/// hit (Tune, Level, Pan, the Gate). In Pitched mode pad 0 is the engine's.
struct PadSetup {
	Kind kind{Kind::Membrane};
	Controls controls {};
	double tuneSemitones{0.0};
	float gain{1.0f}; ///< Level, linear
	float pan{0.0f};  ///< −1 left .. 1 right
	bool gate{false}; ///< Trigger = Gate: a released key damps the hit to Release
	double releaseSeconds{0.1};
	/// BEND (decision 8, 2026-11-07): the share of what moves the voice past its key (wheel, expression, a pitch route) that
	/// moves this pad in Kit mode, held at the strike or moved while it rings. 0 ignores it; Pitched mode follows the voice.
	double bendAmount{1.0};
	/// THE LAYER SLOT (drum proposal §12, stage (c)): a second hit struck with this pad, Kit mode alone. It rides the pad's voice
	/// (its gate, its choke, its pan and Level) and reads the pad's controls through its own kind, at its own place's fields.
	struct Layer {
		bool on{false};
		Kind kind{Kind::Kick};
		float gain{1.0f};          ///< on top of the pad's Level, linear
		double delaySeconds{0.0};  ///< after the pad's strike
		double highPassHz{20.0};   ///< a one-pole high-pass; at 20 Hz (its bottom) bypassed
	} layer {};
};
/// THE WHOLE KIT, owned by the synth and read by every voice (one pointer, set once).
struct KitSetup {
	std::array<PadSetup, 8> pads {};
	bool kit{true};
	/// Whether any pad is panned off centre: the chunk then takes the stereo road (the voice writes both ears).
	bool panned{false};
	/// THE KIT-WIDE TRANSPOSE in semitones (the Engine rail's Oct, St, Coarse and Fine): every pad moves by it in full, never
	/// scaled by a pad's Bend.
	double transposeSemitones{0.0};
};

/// A KIND'S RESTING PITCH at Tune 0 in Kit mode, in Hz (in Pitched mode the key's frequency replaces it).
double kindBaseHz(Kind k);
/// The most a full Sweep drops, in octaves.
double kindSweepOctaves(Kind k);

inline constexpr int kMembraneModes = 16;
inline constexpr int kSnareHeadModes = 8;
inline constexpr int kMetalOscillators = 6;
inline constexpr int kMaxClapBursts = 8;
/// The circular membrane's mode ratios (Bessel zeros j_mn / j_01), lowest first.
extern const std::array<double, kMembraneModes> kMembraneRatios;
/// A free-free bar's mode ratios, lowest first.
extern const std::array<double, kMembraneModes> kBarRatios;
extern const std::array<double, kMembraneModes> kPlateRatios;     ///< a clamped circular plate's, (λ / λ_01)²
extern const std::array<double, kMembraneModes> kStretchedRatios; ///< the bar's to the power 1.25
/// The Metal kind's six ratios at Metal 0.5 (the unspread cluster).
extern const std::array<double, kMetalOscillators> kMetalRatios;
/// −160 dB, the floor below which every envelope and mode is exactly zero.
inline constexpr float kSilenceFloor = 1.0e-8f;
/// The control block, in samples: pitch, decay and filters move at most this often.
inline constexpr int kControlBlock = 16;

/// ONE HIT'S RESOLVED LAWS (proposal §3.1's mapping in one place: the voice and the pictures read it).
struct Laws {
	double hz{110.0};       ///< the resting fundamental, Pitch applied
	double t60{0.5};        ///< seconds, Length applied
	double sweepOctaves{0.0};
	double sweepTau{0.02};  ///< seconds
	float tone{0.5f};       ///< velocity applied
	float noise{0.0f};
	float snap{0.5f};       ///< velocity applied
	float metal{0.0f};
	double drive{0.0};
	int count{3};
	double spreadSeconds{0.03};
	// THE SNARE'S (Controls' same names; Length scales the wires' T60 as it does the body's; the gates are in seconds, 0: none).
	double wireT60{0.18};
	double clickT60{0.006};
	double bodyGate{0.0};
	double wireGate{0.0};
	double wireSweepOctaves{0.0};
	double bellStartHz{2500.0};
	double bellEndHz{700.0};
	double bellTau{0.04 / 3.0};
	double bellQ{6.0};
	double bellLevel{0.0};
	// THE KICK'S (Controls' same names, in seconds).
	double kickHold{0.0};
	double kickDip{0.0};
	double kickDipTime{0.014};
	double clickSweepOctaves{0.0};
	double clickSweepTau{0.001};
	double startPhase{0.0}; ///< radians
	int driveCurve{0};      ///< 0 Tanh, 1 Tube, 2 Soft Clip
	double driveTone{0.5};  ///< the tilt before the drive; 0.5 is flat
	double clapQ{1.6};
	double clapHighPassHz{20.0};
	bool bell2Linked{true};
	double bell2Ratio{1.6};
	double bell2Hz{500.0};
	double bell2T60{0.03};
	double bell2Q{2.0};
	double bell2Level{0.0};
	double malletSeconds{0.0012}; ///< the strike's contact time (Snap, velocity applied)
	int wireContour{0};
	double width{0.0};
	double buzz{0.0};
	double buzzThreshold{0.5};
	friend bool operator==(const Laws&, const Laws&) = default;
};

/// The mapping: Hz = baseHz · 2^((pitch − 0.5)·2·range / 12); T60 = Decay · 4^(2·length − 1); Sweep = sweep · the kind's
/// octaves (a Kick's deepened by velocity); Tone and Snap moved by velocity (0..1) by velocityTone, clamped.
Laws lawsFor(Kind k, const Fields& f, const Controls& c, double baseHz, double velocity);

/// A mode's ratio at Metal m (the structure morph): Bessel at 0, a clamped plate at 1/3, a free bar at 2/3, the bar stretched
/// at 1, geometrically between each pair.
double modeRatio(int k, float metal);
/// A Metal oscillator's ratio at Metal m: kMetalRatios[i]^(0.6 + 0.8·m).
double metalRatio(int i, float metal);
/// MATERIAL (doc §13 item 2): a mode's T60 against the body's, t60 · ratio^−m, the exponent m rising with Metal from 0.6
/// (a skin, the old law) to 2 (wood: the upper modes die at once).
double materialExponent(float metal);
double modeT60(double t60, double ratio, float metal);
/// The band-pass centres, in Hz, from Tone: the Snare's wires (tracking its pitch, Plaits' 16·f0, Tone an octave either
/// way), the Metal's band, the Clap's band.
double wireCentreHz(float tone, double hz);
double metalCentreHz(float tone);
double clapCentreHz(float tone);
/// THE DRIVE'S TRANSFER (every kind): what curve `curve` (0 Tanh, 1 Tube, 2 Soft Clip) at Drive `drive` makes of `x`, in
/// double: the voice runs the same shape in float, and the panel draws its envelope's knee through this. 0 in is 0 out; 1 in is
/// 1 out; Drive 0 is the identity.
double driveTransfer(int curve, double drive, double x);
/// THE MALLET'S WEIGHT on a mode at x = its frequency × the contact time: the raised-cosine pulse's spectrum, 1 at DC.
double malletWeight(double x);
/// The clap's burst onsets, in samples from the strike, for `count` bursts over `spread` seconds jittered by `seed`.
/// Onset 0 is always 0; each later one is i·spread/(count−1)·(1 + 0.3·u_i), u_i uniform in [−0.5, 0.5) from the seed.
std::array<int, kMaxClapBursts> clapOnsets(int count, double spreadSeconds, double sampleRate, uint64_t seed);

/// One exponential envelope: v ← v·d per sample, snapped to exactly 0 below the floor.
struct Env {
	float v{0.0f};
	float d{1.0f};
	void set(float value, double t60Seconds, double sampleRate);
	float next() {
		const float out = v;
		v *= d;
		if (v < kSilenceFloor) {
			v = 0.0f;
		}
		return out;
	}
};

/// A TPT state-variable filter (Simper), band-pass out; its coefficients set once a control block.
struct Svf {
	float g{0.0f}, k{1.0f}, a1{0.0f}, a2{0.0f}, a3{0.0f};
	float ic1{0.0f}, ic2{0.0f};
	void setBandPass(double hz, double q, double sampleRate);
	float bandPass(float x) {
		const float v3 = x - ic2;
		const float v1 = a1 * ic1 + a2 * v3;
		const float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.0f * v1 - ic1;
		ic2 = 2.0f * v2 - ic2;
		return v1;
	}
	void clear() { ic1 = ic2 = 0.0f; }
};

class Hit {
public:
	/// Set the sample rate. Never allocates.
	void prepare(double sampleRate);
	double sampleRate() const { return sr_; }
	/// THE STRING KIND's string and pluck builder, the caller's (prepared at activate). Null: a String hit is silent.
	void setString(ks::KsString* string, ks::PluckBuilder* builder) {
		string_ = string;
		builder_ = builder;
	}

	/// STRIKE: start a hit of kind `k` with laws `l`, its noise from `seed`, at `level` (velocity's gain). A strike on a
	/// ringing hit starts the new one at once (the voice's steal fade is what smooths a retrigger).
	void strike(Kind k, const Laws& l, uint64_t seed, float level);
	/// The laws for every control block from the next (a moving field or a modulated control); the kind stays latched.
	void setLaws(const Laws& l);
	/// THE GATE (proposal D4): from the next sample every remaining envelope and mode decays to `t60` if that is shorter.
	void release(double t60Seconds);
	/// Write `n` samples. Exact zeros once idle.
	/// Render `n` samples into `out`; with `outR`, the right ear too (it differs from the left only by Width's decorrelated
	/// noise on a Snare's wires or a Clap's band).
	void render(float* out, int n, float* outR = nullptr);
	/// Silence, every state zeroed: idle.
	void reset();
	bool idle() const { return idle_; }

	// ---- TEST SEAMS: values, not buffers (CLAUDE.md) -------------------------------------------------------------------
	Kind kind() const { return kind_; }
	const Laws& laws() const { return laws_; }
	/// The frequency, in Hz, the Kick's body or mode k (Membrane/Snare) is rotating at NOW (from the increment in use).
	double currentHz(int mode = 0) const;
	/// Mode k's present amplitude |z|, and the per-sample decay its rotator is applying (|r|).
	float modeAmplitude(int k) const;
	double modeDecayPerSample(int k) const;
	int liveModes() const { return modeCount_; }
	/// The Metal kind's oscillator i's frequency in Hz.
	double metalHz(int i) const;
	/// The band-pass's centre in Hz, as last set.
	double bandCentreHz() const { return bandHz_; }
	/// The Snare's: the moving bell's centre now, whether its head and wires are past their gates, the wires' decay per sample.
	double bellHzForTest() const { return bellHz_; }
	double bell2HzForTest() const { return bell2Hz_; }
	/// This hit's seeded ±2% on its tension glide's depth (1 on a kind without Tension).
	double tensionVariationForTest() const { return kindControls(kind_).tension ? tensionVariation_ : 1.0; }
	/// Mode k's phase now (its start phase, read straight after a strike).
	double modePhaseForTest(int k) const {
		return std::atan2(static_cast<double>(zi_[static_cast<size_t>(k)]), static_cast<double>(zr_[static_cast<size_t>(k)]));
	}
	/// The second bell's gain as the last rendered sample used it (its level times its decaying envelope, ×4 at full).
	float bell2GainForTest() const { return bell2Used_; }
	bool bodyGatedForTest() const { return bodyGated_; }
	bool wireGatedForTest() const { return wireGated_; }
	float wireDecayPerSampleForTest() const { return band_.d; }
	float clickDecayPerSampleForTest() const { return click_.d; }
	/// The Kick's: the fishtail's gain now, whether the body holds, the body's decay per sample, the sine's phase (radians).
	float dipGainForTest() const { return dipGain_; }
	bool holdingForTest() const { return holding_; }
	float bodyDecayPerSampleForTest() const { return body_.d; }
	double kickPhaseForTest() const { return std::atan2(static_cast<double>(ks_), static_cast<double>(kc_)); }
	int driveCurveForTest() const { return driveCurve_; }
	float driveGainForTest() const { return driveGain_; }
	float driveTiltForTest() const { return driveTilt_; }
	double clapQForTest() const { return bp_.k > 0.0f ? 1.0 / static_cast<double>(bp_.k) : 0.0; }
	float clapHighPassCoefForTest() const { return clapHpCoef_; }
	/// The wires' envelope as the last sample used it (its contour and its buzz applied), and the buzz's factor alone.
	float wireEnvForTest() const { return wireEnvUsed_; }
	float buzzForTest() const { return buzzUsed_; }
	/// The right ear minus the left at the last sample (Width's decorrelated noise).
	float sideForTest() const { return side_; }
	/// The head's own output at the last sample (the modes alone, what the wires buzz against), and the buzz's follower of it.
	float headForTest() const { return lastHead_; }
	float buzzFollowForTest() const { return buzzFollow_; }
	/// The clap's tooth as the last rendered sample USED it (the voice's own number, stored where it is made), before the noise
	/// and filters; 0 once the tooth has landed.
	float clapToothForTest() const { return toothValue_; }
	double dipAtForTest(double t) const { return dipAt(t); }
	const std::array<int, kMaxClapBursts>& clapOnsetsForTest() const { return onsets_; }
	/// Samples since the strike.
	int64_t age() const { return age_; }
	uint64_t seed() const { return seed_; }

private:
	void control();
	void setModes(int count, double bend);
	float renderKick();
	float renderModes();
	float renderMetal();
	float renderClap();
	float clapTooth();
	float noise() { return noise_.next(); }
	bool envelopesSilent() const;
	void checkSilence();
	/// A T60 after the Gate: the shorter of it and Release once released.
	double effective(double t60) const;
	/// What the last control block was computed from: an equal key is a block with nothing to recompute.
	struct ControlKey {
		Laws laws {};
		double bend{0.0};
		bool released{false};
		double releaseT60{0.0};
		double wireBend{0.0};
		double bellHz{0.0};
		bool bodyGated{false};
		bool wireGated{false};
		bool holding{false};
		double dip{1.0};
		friend bool operator==(const ControlKey&, const ControlKey&) = default;
	};
	ControlKey lastControl_ {};
	bool controlValid_{false};
	/// The snare's: whether the head and the wires are past their gates, the moving bell's filter and its centre now.
	bool bodyGated_{false};
	bool wireGated_{false};
	Svf bell_ {};
	double bellHz_{0.0};
	bool bellOn_{false};
	Svf bell2_ {};
	double bell2Hz_{0.0};
	bool bell2On_{false};
	float bell2Gain_{0.0f};
	float bell2Used_{0.0f};
	Env bell2Env_ {};
	float bellGain_{0.0f};

	double sr_{48000.0};
	Kind kind_{Kind::Membrane};
	Laws laws_ {};
	bool idle_{true};
	int64_t age_{0};
	uint64_t seed_{1};
	float level_{1.0f};
	/// This hit's Tone multiplier from its seed (proposal D9), applied to every Laws it is handed.
	double toneVariation_{1.0};
	double tensionVariation_{1.0}; ///< ±2% on the tension glide's depth, seeded (Kick, Membrane, Snare)
	int controlLeft_{0};
	double releaseT60_{0.0};
	bool released_{false};
	ks::PluckNoise noise_{1};

	// The body's amplitude, the click's and the noise band's (each kind uses those it needs).
	Env body_ {};
	Env click_ {};
	Env band_ {};
	Env tail_ {};

	// Kick: the phase, and the increment in use (cycles per sample).
	double phase_{0.0};
	double inc_{0.0};
	/// The body's (cos, sin) and the per-sample rotation; the drive's gain and its normaliser.
	float kc_{1.0f};
	float ks_{0.0f};
	float rotC_{1.0f};
	float rotS_{0.0f};
	float driveGain_{0.0f};
	float driveNorm_{1.0f};
	int driveCurve_{0};
	float driveTilt_{0.0f};  ///< the tone before the drive, −1 (lows) .. 1 (highs); 0 is flat and skipped
	float driveLp_{0.0f};
	float driveLpCoef_{0.0f};
	float driveBias_{0.0f};  ///< Tube's lean
	float driveOffset_{0.0f};///< tanh(bias), so 0 in is exactly 0 out
	void driveSpan(float* out, int n, float& lp);
	/// THE KICK'S SHAPE: the fishtail's gain this block and its per-sample step (a ramp to the next block's), and whether the
	/// body is still holding.
	float dipGain_{1.0f};
	float dipStep_{0.0f};
	bool holding_{false};
	double dipAt(double t) const;

	// Membrane and Snare: the modal bank. z = amplitude·e^{iθ}, c = r·e^{iω}, out = Im(z).
	int modeCount_{0};
	std::array<float, kMembraneModes> zr_ {};
	std::array<float, kMembraneModes> zi_ {};
	std::array<float, kMembraneModes> cr_ {};
	std::array<float, kMembraneModes> ci_ {};
	std::array<double, kMembraneModes> modeHz_ {};
	std::array<double, kMembraneModes> modeR_ {};
	/// The cache setModes keeps: each mode's ratio at `ratioMetal_`, and its per-sample decay at `decayT60_`.
	std::array<double, kMembraneModes> ratio_ {};
	std::array<double, kMembraneModes> decayR_ {};
	float ratioMetal_{-1.0f};
	int ratioCount_{0};
	double decayT60_{-1.0};

	// Metal: six phases and increments.
	std::array<double, kMetalOscillators> mphase_ {};
	std::array<double, kMetalOscillators> minc_ {};
	/// The cluster in float, eight lanes (the six and two silent, weight 0) so the per-sample loop vectorises.
	static constexpr size_t kLanes = 8;
	alignas(32) std::array<float, kLanes> mphaseF_ {};
	alignas(32) std::array<float, kLanes> mincF_ {};
	alignas(32) std::array<float, kLanes> invDtF_ {};
	alignas(32) std::array<float, kLanes> laneW_ {};
	float hpState_{0.0f};
	float hpCoef_{0.0f};

	// The click's one-pole lowpass (Snap: its corner).
	float clickLp_{0.0f};
	float clickCoef_{1.0f};

	Svf bp_ {};
	// THE SNARE'S WIRES (doc §13 items 7 and 9): their contour, their buzz, and Width's second, decorrelated wires for the right.
	int contour_{0};
	float wireLin_{0.0f};
	float wireLinStep_{0.0f};
	float modPhase_{0.0f};
	float modInc_{0.0f};
	float buzzFollow_{0.0f};
	float buzzCoef_{0.0f};
	float buzzAmount_{0.0f};
	float buzzKnee_{0.0f};
	float buzzSpan_{1.0f};
	float buzzUsed_{1.0f};
	bool widthOn_{false};
	float widthC_{1.0f};
	float widthS_{0.0f};
	ks::PluckNoise noiseR_{1};
	Svf bpR_ {};
	float side_{0.0f};
	float wireEnvUsed_{0.0f};
	float lastHead_{0.0f};
	float clapHpLpR_{0.0f};
	float driveLpR_{0.0f};
	float wires(float head);
	float clapNoise();
	double bandHz_{0.0};

	// Clap.
	std::array<int, kMaxClapBursts> onsets_ {};
	int nextBurst_{0};
	// THE CLAP'S TEETH (the 808's per-burst sawtooth): the tooth's position in samples across its span; at its span
	// the tooth is done (toothOn_ false) and only the tail sounds.
	int toothK_{0};       ///< samples into the tooth (counted, so it lands exactly at its span)
	int toothSpan_{1};
	float toothInv_{0.0f}; ///< 1 / span
	float toothValue_{0.0f}; ///< the tooth the last sample used (its seam)
	bool toothOn_{false};
	float clapHpLp_{0.0f};
	float clapHpCoef_{0.0f};
	float clapGain_{3.0f}; ///< 3 at Q 1.6, and ∝ 1/√Q: a narrower band passes less noise, so its loudness holds as Q moves

	ks::KsString* string_{nullptr};
	ks::PluckBuilder* builder_{nullptr};
	double stringHz_{-1.0};
};

} // namespace feathers::drum
