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

#include "dsp/drum_voice.h"

#include "dsp/fast_math_approx.h"

// FEATHERS_FORCE_SIMDE takes the arm64 road on x86-64 too (the partial bank's preamble, so clang-preflight compiles what macOS does).
#if !defined(FEATHERS_FORCE_SIMDE)                                                                        \
    && (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#endif

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::drum {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
/// The highest frequency any part of a hit is allowed, as a fraction of the sample rate: a mode above it is silent, a
/// filter's centre is clamped to it.
constexpr double kTopFraction = 0.45;
/// HOW FAST A GATED COMPONENT FALLS once past its Shape · T60 (drum proposal §12): a T60 of 15 ms, a cut without a click.
constexpr double kGateFall = 0.015;
/// The drive's Tube curve leans by this bias (its even harmonics); the tilt before the drive pivots about this pole.
constexpr float kTubeBias = 0.35f;
constexpr double kDriveTiltHz = 600.0;

/// The zeros j_mn of the Bessel functions J_m, lowest first: the circular membrane's modes are at j_mn / j_01.
constexpr std::array<double, kMembraneModes> kBesselZeros { 2.404825557695773, 3.831705970207512, 5.135622301840683,
	                                                         5.520078110286311, 6.380161895923984, 7.015586669815619,
	                                                         7.588342434503805, 8.417244140399865, 8.653727912911012,
	                                                         8.771483815959954, 9.761023129981670, 9.936109524217684,
	                                                         10.17346813506272, 11.06470948850119, 11.08637001924291,
	                                                         11.61984117214906 };

std::array<double, kMembraneModes> makeMembrane() {
	std::array<double, kMembraneModes> r {};
	for (size_t k = 0; k < r.size(); ++k) {
		r[k] = kBesselZeros[k] / kBesselZeros[0];
	}
	return r;
}

/// A free-free bar: β_n L for the first six roots of cos β cosh β = 1, then (2n + 1)π/2, which they approach; the ratios
/// are (β_n / β_1)².
std::array<double, kMembraneModes> makeBar() {
	constexpr std::array<double, 6> beta { 4.730040744862704, 7.853204624095838, 10.99560783800167, 14.13716549125746,
		                                   17.27875965739948, 20.42035224562606 };
	std::array<double, kMembraneModes> r {};
	for (size_t k = 0; k < r.size(); ++k) {
		const double b = k < beta.size() ? beta[k] : (2.0 * static_cast<double>(k + 1) + 1.0) * std::numbers::pi / 2.0;
		r[k] = (b / beta[0]) * (b / beta[0]);
	}
	return r;
}


/// Bessel J_n and modified Bessel I_n by their power series (enough terms for x ≤ 14 in double): used once, at load, to find
/// the clamped plate's modes, so nothing here depends on a library's special functions (libc++ has none).
double besselSeries(int n, double x, bool modified) {
	const double h = 0.5 * x;
	double term = 1.0;
	for (int i = 1; i <= n; ++i) {
		term *= h / static_cast<double>(i);
	}
	double sum = term;
	for (int k = 1; k < 80; ++k) {
		term *= (modified ? 1.0 : -1.0) * h * h / (static_cast<double>(k) * static_cast<double>(k + n));
		sum += term;
		if (std::abs(term) < 1.0e-18 * std::abs(sum)) {
			break;
		}
	}
	return sum;
}

/// A CLAMPED CIRCULAR PLATE (the structure morph's second station, doc §13 item 5): its modes are the roots λ of
/// J_n(λ)·I_{n+1}(λ) + I_n(λ)·J_{n+1}(λ) = 0 over n, and a plate's frequency goes as λ², so the ratios are (λ / λ_01)².
/// Found by a scan and bisection at load; the first are 1, 2.081, 3.414, 3.893 (Leissa, "Vibration of Plates").
std::array<double, kMembraneModes> makePlate() {
	std::array<double, 64> roots {};
	size_t count = 0;
	for (int n = 0; n < 12; ++n) {
		const auto g = [n](double x) {
			return besselSeries(n, x, false) * besselSeries(n + 1, x, true)
			       + besselSeries(n, x, true) * besselSeries(n + 1, x, false);
		};
		double a = 0.5;
		double ga = g(a);
		for (double b = a + 0.01; b <= 14.0 && count < roots.size(); b += 0.01) {
			const double gb = g(b);
			if ((ga < 0.0) != (gb < 0.0)) {
				double lo = b - 0.01;
				double hi = b;
				double glo = ga;
				for (int it = 0; it < 60; ++it) {
					const double mid = 0.5 * (lo + hi);
					const double gm = g(mid);
					if ((glo < 0.0) == (gm < 0.0)) {
						lo = mid;
						glo = gm;
					}
					else {
						hi = mid;
					}
				}
				roots[count++] = 0.5 * (lo + hi);
			}
			ga = gb;
		}
	}
	std::sort(roots.begin(), roots.begin() + static_cast<std::ptrdiff_t>(count));
	std::array<double, kMembraneModes> r {};
	for (size_t k = 0; k < r.size(); ++k) {
		const double l = k < count ? roots[k] : roots[count - 1] * (1.0 + 0.1 * static_cast<double>(k - count + 1));
		r[k] = (l / roots[0]) * (l / roots[0]);
	}
	return r;
}
/// Per-sample decay factor for a T60 (≤ 0: at once).
double decayFactor(double t60Seconds, double sampleRate) {
	if (!(t60Seconds > 0.0)) {
		return 0.0;
	}
	if (!std::isfinite(t60Seconds)) {
		return 1.0;
	}
	return std::pow(10.0, -3.0 / (t60Seconds * sampleRate));
}

/// The same, as an envelope multiplies it.
float envDecay(double t60Seconds, double sampleRate) { return static_cast<float>(decayFactor(t60Seconds, sampleRate)); }

float clamp01(double x) { return static_cast<float>(std::clamp(x, 0.0, 1.0)); }

/// THE PER-HIT VARIATION (proposal D9): Tone within ±3% and level within ±0.25 dB, from the hit's own seed.
constexpr double kToneVariation = 0.03;
constexpr double kLevelVariationDb = 0.25;
constexpr uint64_t kVariationSalt = 0xD7E3A5F00DC0FFEEull;
constexpr uint64_t kClapSalt = 0xC1A9C1A9B0B5EEDull;
/// The modes' start phases (doc §13 item 10): their own stream, so the tone, level and clap draws are as they were.
constexpr uint64_t kPhaseSalt = 0x9A5E0F5E7D1CEull;
constexpr double kModePhaseJitter = 0.1;
/// A HAND DRUM'S CEILING: the head, the shell and the hand's tap add, so the sum may pass full scale. Linear up to 0.8, a tanh knee above it that tends to 1, 0 in 0 out.
float handCeiling(float x) {
	constexpr float kKnee = 0.8f;
	const float a = std::fabs(x);
	if (a <= kKnee) {
		return x;
	}
	const float y = kKnee + (1.0f - kKnee) * dsp::fasttanh(std::min((a - kKnee) / (1.0f - kKnee), 4.5f));
	return x < 0.0f ? -y : y;
}
/// The shell's mode at full Shell, against the head's modes (which sum to the strike's level).
constexpr double kHandShellGain = 0.8;
/// The FM perc's carrier gain: the carrier is a unit sine, so the tap and the noise share fit above it.
constexpr double kFmGain = 0.8;
/// A Drone's: a held tone sits under a mix rather than striking it, so its default, through the default Drive, reads between -26 and -12 dBFS RMS like the other held kinds.
constexpr double kFmDroneGain = 0.08;
/// ±2% of the tension glide's depth, seeded (doc §13 item 10).
constexpr double kTensionVariation = 0.02;
/// A tension glide's resolution: half a cent (1/2400 of an octave).
constexpr double kTensionSteps = 2400.0;
/// Width's second noise (the right ear's wires or clap band): its own stream, so the left is the mono hit to the bit.
constexpr uint64_t kWidthSalt = 0x5E7E0D1DE0ull;
constexpr uint64_t kParticleSalt = 0x9A27C1E5B0A7ull;
constexpr uint64_t kFrictionSalt = 0xF1C7107B0A7ull;
constexpr float kFrictionGain = 10.0f; ///< the resonators' sum to the level of the other kinds (measured, test_drum_voice)
constexpr uint64_t kWaterSalt = 0x3A7E2B0B91Eull;
constexpr uint64_t kSweepSalt = 0x5EE9B0A7C1ull;
constexpr float kParticleGain = 0.35f; ///< the resonances' sum to a hit of about the level of the other kinds (measured, test_drum_voice)
/// The Modulated contour: the wires' envelope chopped by a falling sawtooth at this rate, to this depth (Microtonic's Mod).
constexpr double kWireModHz = 45.0;
constexpr float kWireModDepth = 0.8f;
/// The buzz follows the head's rectified swing with this time constant; full Threshold is this far up the swing.
constexpr double kBuzzFollowSeconds = 0.001;
constexpr double kBuzzFullSwing = 0.5;

} // namespace

const std::array<double, kMembraneModes> kMembraneRatios = makeMembrane();
const std::array<double, kMembraneModes> kBarRatios = makeBar();
const std::array<double, kMembraneModes> kPlateRatios = makePlate();
/// THE STRETCHED STATION, past the bar: its ratios to the power 1.25, stiffer and further from harmonic than any bar.
const std::array<double, kMembraneModes> kStretchedRatios = [] {
	std::array<double, kMembraneModes> r {};
	for (size_t k = 0; k < r.size(); ++k) {
		r[k] = std::pow(kBarRatios[k], 1.25);
	}
	return r;
}();
const std::array<double, kMetalOscillators> kMetalRatios { 1.0, 1.4826, 1.8003, 2.5461, 2.6303, 3.8968 };

FieldUse kindReads(Kind k) {
	switch (k) {
	case Kind::Kick: return { true, true, true, true, true, true, false };
	case Kind::Membrane: return { true, true, true, true, true, true, true };
	case Kind::Snare: return { true, true, true, true, true, true, true };
	case Kind::Metal: return { true, true, false, true, true, true, true };
	case Kind::Clap: return { true, true, false, true, true, true, false };
	case Kind::String: return { true, true, true, true, false, true, true };
	// PARTICLE: the resonance's pitch, the shower's length (its Decay), the grains' brightness, the raw noise share, how unequal the grains are, and the
	// resonances' spread. No sweep: nothing glides.
	case Kind::Particle: return { true, true, false, true, true, true, true };
	// FRICTION: the resonator's pitch, its ring (Decay), the slip's brightness (Tone), the roughness (Noise), the slip's edge (Snap), and the second resonance's place (Metal).
	case Kind::Friction: return { true, true, false, true, true, true, true };
	// WATER: the size (Pitch), the shower's length, the splash's and the bed's colour (Tone), the chirp's ring (Snap) and the spread of sizes and places
	// (Metal). Nothing glides by the Sweep field (Rise is its own control) and there is no Noise share (the Flow control is the bed's level).
	case Kind::Water: return { true, true, false, true, false, true, true };
	// SWEEP: the low corner's pitch, the length (its Decay), how far the oscillator follows the filter (Sweep), the filter's type (Tone), the noise against
	// the oscillator (Noise) and the resonance (Snap). No Metal.
	case Kind::Sweep: return { true, true, true, true, true, true, false };
	// HAND DRUM: the pitch, the ring (Decay), the bend's depth (Sweep), the strike position (Tone), the skin's and fingers' noise, the contact's hardness (Snap) and the
	// head's inharmonicity (Metal). FM PERC: the carrier's pitch, the amplitude's ring, the pitch envelope's depth (Sweep), the index's brightness (Tone), the
	// noise transient's share, its hardness (Snap) and the ratio's inharmonic lean (Metal).
	case Kind::Hand:
	case Kind::FM: return { true, true, true, true, true, true, true };
	default: return { false, false, false, false, false, false, false };
	}
}

ControlUse kindControls(Kind k) {
	ControlUse u {};
	// DRIVE ON EVERY KIND, with its curve and the tone before it.
	u.drive = k != Kind::kCount;
	// SWEEP TIME: the Kick's pitch envelope and the String's; a Membrane's or a Snare's glide follows its own ring (Tension).
	u.sweepTime = k == Kind::Kick || k == Kind::String || k == Kind::Hand || k == Kind::FM;
	u.count = u.spread = u.clapFilter = k == Kind::Clap;
	u.snareEnvelopes = u.bell = k == Kind::Snare;
	u.kickShape = k == Kind::Kick;
	u.tension = k == Kind::Kick || k == Kind::Membrane || k == Kind::Snare || k == Kind::Hand;
	u.velNoise = k != Kind::kCount && kindReads(k).noise;
	u.wireContour = k == Kind::Snare;
	u.width = k == Kind::Snare || k == Kind::Clap;
	return u;
}

double kindBaseHz(Kind k) {
	switch (k) {
	case Kind::Kick: return 50.0;
	case Kind::Membrane: return 110.0;
	case Kind::Snare: return 180.0;
	case Kind::Metal: return 400.0;
	case Kind::Clap: return 1000.0;
	case Kind::String: return 110.0;
	case Kind::Particle: return 3000.0;
	case Kind::Friction: return 600.0;
	case Kind::Water: return 900.0;
	case Kind::Sweep: return 200.0;
	case Kind::Hand: return 200.0;
	case Kind::FM: return 220.0;
	default: return 110.0;
	}
}

double kindSweepOctaves(Kind k) {
	switch (k) {
	case Kind::Kick: return 4.0;
	case Kind::Membrane: return 1.0;
	case Kind::Snare: return 0.5;
	case Kind::String: return 1.0;
	case Kind::Sweep: return 1.0; // the Sweep field is how far the oscillator follows the filter (0..1)
	case Kind::Hand: return 1.0;  // the bend's depth, up or down
	case Kind::FM: return 3.0;    // the pitch envelope's depth, falling onto the pitch
	default: return 0.0;
	}
}

namespace {

struct ZoneSpot {
	size_t at;
	size_t next;
	double frac;
};

/// Where a coordinate stands among `n` zones: the zone it is in, the next, and how far across. **IT STOPS AT BOTH ENDS**: the zones sit at
/// k/n, so the last is at (n-1)/n and the coordinate is held to 0..(n-1)/n — past the last zone is the last zone, never the first again
/// (it used to take the coordinate's fractional part, so one lap on wrapped a Snare's Gated into Tight).
ZoneSpot zoneSpot(double coordinate, size_t n) {
	const double top = static_cast<double>(n - 1) / static_cast<double>(n);
	const double u = std::isfinite(coordinate) ? std::clamp(coordinate, 0.0, top) : 0.0;
	const double z = u * static_cast<double>(n);
	const size_t at = std::min(static_cast<size_t>(std::max(0.0, std::floor(z))), n - 1);
	const size_t next = std::min(at + 1, n - 1);
	return { at, next, next == at ? 0.0 : z - static_cast<double>(at) };
}


double lerp(double a, double b, double t) { return a + (b - a) * t; }

constexpr std::array<WiresMix, 4> kWiresAnchors { {
    { 0, 0.0, 0.0, 0.0, 0.5 },   // Tight: the wires as they were, an exponential fall, nothing gated, no buzz
    { 1, 0.0, 0.0, 0.3, 0.7 },   // Loose: a straight fall, a little buzz on the hardest hits
    { 2, 0.0, 0.0, 1.0, 0.3 },   // Buzzy: rattling in teeth, the wires following the head from a light touch
    { 1, 0.45, 0.35, 0.4, 0.5 }, // Gated: head and wires cut short, the 80s gated snare
} };

constexpr std::array<BellsMix, 5> kBellsAnchors { {
    { 0.0, 0.0, 1.0, 1.0, true },  // Off
    { 0.5, 0.0, 1.0, 1.0, true },  // Body: one moving peak, the head's
    { 0.5, 0.35, 1.0, 1.0, true }, // Shell: and the shell's, linked above it
    { 0.4, 0.3, 3.0, 3.0, true },  // Ring: both narrow and ringing
    { 0.6, 0.4, 0.6, 0.6, false }, // Knock: wide, and the second free at its own Hz
} };

constexpr std::array<VelocityMix, 3> kVelocityAnchors { {
    { 0.0, 0.0 }, // Level: velocity is only loudness
    { 0.5, 0.0 }, // Tone: and brightness and snap
    { 0.5, 1.0 }, // Tension: and the glide's depth, by velocity squared
} };

} // namespace

double levelFor(double velocity, double depth) {
	const double d = std::isfinite(depth) ? std::clamp(depth, 0.0, 1.0) : 1.0;
	return std::clamp(1.0 - d * (1.0 - std::clamp(velocity, 0.0, 1.0)), 0.0, 1.0);
}

WiresMix wiresAt(double coordinate) {
	const ZoneSpot s = zoneSpot(coordinate, kWiresAnchors.size());
	const WiresMix& a = kWiresAnchors[s.at];
	const WiresMix& b = kWiresAnchors[s.next];
	return { s.frac < 0.5 ? a.contour : b.contour, lerp(a.wireShape, b.wireShape, s.frac), lerp(a.bodyShape, b.bodyShape, s.frac),
		     lerp(a.buzz, b.buzz, s.frac), lerp(a.threshold, b.threshold, s.frac) };
}

BellsMix bellsAt(double coordinate) {
	const ZoneSpot s = zoneSpot(coordinate, kBellsAnchors.size());
	const BellsMix& a = kBellsAnchors[s.at];
	const BellsMix& b = kBellsAnchors[s.next];
	return { lerp(a.level1, b.level1, s.frac), lerp(a.level2, b.level2, s.frac), lerp(a.q1Scale, b.q1Scale, s.frac),
		     lerp(a.q2Scale, b.q2Scale, s.frac), s.frac < 0.5 ? a.linked : b.linked };
}

VelocityMix velocityAt(double coordinate) {
	const ZoneSpot s = zoneSpot(coordinate, kVelocityAnchors.size());
	const VelocityMix& a = kVelocityAnchors[s.at];
	const VelocityMix& b = kVelocityAnchors[s.next];
	return { lerp(a.tone, b.tone, s.frac), lerp(a.tension, b.tension, s.frac) };
}

double wireSweepFor(float sweepField) { return static_cast<double>(std::clamp(sweepField, 0.0f, 1.0f)); }
double bellQFor(float snap, double scale) { return (2.0 + 10.0 * static_cast<double>(std::clamp(snap, 0.0f, 1.0f))) * scale; }
double bell2QFor(float snap, double scale) { return (1.0 + 6.0 * static_cast<double>(std::clamp(snap, 0.0f, 1.0f))) * scale; }
double bell2RatioFor(float metal) { return 1.3 + 1.2 * static_cast<double>(std::clamp(metal, 0.0f, 1.0f)); }

double terrainPitchSemitones(float fieldPitch, const Controls& c) {
	// THE TERRAIN'S PITCH, in semitones either side of the resting pitch, under the layer's mode. The Free road is the product
	// the engine always formed, in the same order, so it is bit-identical to before the modes.
	const double semitones = (static_cast<double>(fieldPitch) - 0.5) * 2.0 * c.pitchRangeSemitones;
	switch (c.pitchMode) {
	case 1: return std::round(semitones);
	case 2: return 0.0;
	default: return semitones;
	}
}

Laws lawsFor(Kind k, const Fields& f, const Controls& c, double baseHz, double velocity) {
	Laws l;
	l.hz = baseHz * std::pow(2.0, terrainPitchSemitones(f.pitch, c) / 12.0);
	l.t60 = c.decaySeconds * std::pow(4.0, 2.0 * static_cast<double>(f.length) - 1.0);
	// THE THREE ZONES, read once (drum proposal §14): what velocity reaches, the wires' character, the bells.
	// (THE DIRECT ROAD: each of these is a number of its own; the zones are the synth engine's, until it goes.)
	const VelocityMix vel = c.direct ? VelocityMix { c.velTone, c.velTension } : velocityAt(c.velocityZone);
	const WiresMix wire = c.direct ? WiresMix { std::clamp(c.wireContour, 0, 2), c.wireShape, c.bodyShape, c.wireBuzz, c.buzzThreshold }
	                               : wiresAt(c.wires);
	const BellsMix bell = c.direct ? BellsMix { c.bell1Level, c.bell2Level, c.bellQScale, c.bellQScale, c.bellLinked } : bellsAt(c.bells);
	const double vtTone = std::clamp(vel.tone, -1.0, 1.0) * (velocity - 0.5);
	const double vtSnap = c.direct ? std::clamp(c.velSnap, -1.0, 1.0) * (velocity - 0.5) : vtTone;
	l.tone = clamp01(static_cast<double>(f.tone) + vtTone);
	l.snap = clamp01(static_cast<double>(f.snap) + vtSnap);
	l.sweepOctaves = static_cast<double>(f.sweep) * kindSweepOctaves(k);
	l.sweepTau = std::max(1.0e-4, c.sweepSeconds / 3.0);
	// THE HAND DRUM'S STROKE moves the hit before anything below reads it: the strike position, the contact, the ring and the bend's reach.
	const HandStroke stroke = k == Kind::Hand ? handStroke(c.handStroke) : handStroke(kStrokeOpen);
	l.tone = clamp01(l.tone + stroke.toneShift);
	l.snap = clamp01(l.snap + stroke.snapShift);
	l.t60 *= stroke.t60Mul;
	l.sweepOctaves *= stroke.bendMul;
	if (k == Kind::Kick || k == Kind::Membrane || k == Kind::Snare || k == Kind::Hand) {
		// TENSION (doc §13 item 1, Avanzini): a struck head's pitch offset goes as its amplitude SQUARED, so the glide's depth
		// is velocity² (a hard hit drops further by itself), Velocity Tension setting how much of that law applies. On a
		// Membrane or a Snare the glide follows the head's own envelope squared, 10^(−6t/T60), so it is as long as the drum
		// rings: no Sweep Time. The Kick keeps its Sweep Time (its pitch envelope is the kick's character).
		const double v = std::clamp(velocity, 0.0, 1.0);
		l.sweepOctaves *= std::max(0.0, 1.0 + std::clamp(vel.tension, 0.0, 1.0) * (v * v - 1.0));
		if (k == Kind::Membrane || k == Kind::Snare) {
			l.sweepTau = l.t60 / (6.0 * std::numbers::ln10);
		}
	}
	// VELOCITY TO NOISE (doc §13 item 4): a harder hit is noisier (or cleaner, below 0), as Velocity moves Tone.
	l.noise = clamp01(static_cast<double>(f.noise) + std::clamp(c.velocityNoise, -1.0, 1.0) * (velocity - 0.5));
	// MALLET STIFFNESS (doc §13 item 3): Snap, velocity applied, is the contact time of the strike, 5 ms (soft) to 0.3 ms (hard).
	l.malletSeconds = 0.005 * std::pow(0.06, static_cast<double>(l.snap));
	l.metal = f.metal;
	l.drive = std::clamp(c.drive, 0.0, 1.0);
	l.count = std::clamp(c.count, 1, kMaxClapBursts);
	l.spreadSeconds = std::max(0.0, c.spreadSeconds);
	// THE SNARE'S ENVELOPES AND BELL (drum proposal §12). Length scales the wires' T60 as it scales the body's; a Shape above 0
	// gates its component at Shape · its T60.
	const double lengthMul = std::pow(4.0, 2.0 * static_cast<double>(f.length) - 1.0);
	l.wireT60 = std::max(1.0e-3, c.wireDecaySeconds) * lengthMul;
	l.clickT60 = std::max(1.0e-4, c.clickDecaySeconds);
	l.bodyGate = wire.bodyShape > 0.0 ? std::clamp(wire.bodyShape, 0.0, 1.0) * l.t60 : 0.0;
	l.wireGate = wire.wireShape > 0.0 ? std::clamp(wire.wireShape, 0.0, 1.0) * l.wireT60 : 0.0;
	l.wireSweepOctaves = wireSweepFor(f.sweep); // yielded to the terrain: the Snare's Tension
	l.bellStartHz = std::max(20.0, c.bellStartHz);
	l.bellEndHz = std::max(20.0, c.bellEndHz);
	l.bellTau = std::max(1.0e-4, c.bellSeconds / 3.0);
	l.bellQ = std::clamp(bellQFor(f.snap, bell.q1Scale), 0.5, 40.0); // yielded to the terrain: the place's Snap
	l.bellLevel = std::clamp(bell.level1, 0.0, 1.0);
	// THE KICK'S SHAPE (drum proposal §12 items 1-3).
	l.kickHold = std::max(0.0, c.kickHoldSeconds);
	l.kickDip = std::clamp(c.kickDip, 0.0, 1.0);
	l.kickDipTime = std::max(1.0e-3, c.kickDipSeconds);
	l.clickSweepOctaves = std::max(0.0, c.clickSweepOctaves);
	l.clickSweepTau = std::max(1.0e-5, c.clickSweepSeconds / 3.0);
	l.startPhase = std::clamp(c.startPhaseDegrees, 0.0, 90.0) * std::numbers::pi / 180.0;
	// THE DRIVE'S CURVE AND TONE (every kind) AND THE CLAP'S FILTER.
	l.driveCurve = std::clamp(c.driveCurve, 0, 2);
	l.driveTone = std::clamp(c.driveTone, 0.0, 1.0);
	l.clapQ = std::clamp(c.clapQ, 0.5, 8.0);
	l.clapHighPassHz = std::clamp(c.clapHighPassHz, 20.0, 2048.0);
	// THE SECOND BELL (Requirement: two moving peaks; doc §13.2).
	l.bell2Linked = bell.linked;
	l.bell2Ratio = bell2RatioFor(f.metal); // yielded to the terrain: the place's Metal
	l.bell2Hz = std::max(20.0, c.bell2Hz);
	l.bell2T60 = std::max(1.0e-3, c.bell2Seconds);
	l.bell2Q = std::clamp(bell2QFor(f.snap, bell.q2Scale), 0.5, 40.0);
	l.bell2Level = std::clamp(bell.level2, 0.0, 1.0);
	// THE WIRES' CONTOUR, WIDTH AND BUZZ (doc §13 items 7 and 9).
	l.wireContour = std::clamp(wire.contour, 0, 2);
	l.width = std::clamp(c.width, 0.0, 1.0);
	l.buzz = std::clamp(wire.buzz, 0.0, 1.0);
	l.buzzThreshold = std::clamp(wire.threshold, 0.0, 1.0);
	l.density = std::clamp(c.particleDensity, 0.25, 2000.0);
	l.grainT60 = std::clamp(c.grainSeconds, 1.0e-4, 0.5);
	l.sweepDirection = c.sweepDirection == 1 ? 1 : 0;
	l.sweepRange = std::clamp(c.sweepRange, 0.25, 9.0);
	l.lengthSeconds = std::max(0.005, c.syncBeats > 0.0 && c.secondsPerBeat > 0.0 ? c.syncBeats * c.secondsPerBeat : l.t60);
	l.waterRate = std::clamp(c.waterRate, 0.0625, 1000.0);
	l.rise = std::clamp(c.rise, 0.0, 1.0);
	l.flow = std::clamp(c.flow, 0.0, 1.0);
	l.splash = std::clamp(c.splash, 0.0, 1.0);
	l.pressure = std::clamp(c.pressure, 0.0, 1.0);
	l.speed = std::clamp(c.speed, 0.0, 1.0);
	l.play = kindSustains(k) ? std::clamp(c.play, kPlayOneShot, kPlayDrone) : (c.play == kPlayGate ? kPlayGate : kPlayOneShot);
	l.handStroke = std::clamp(c.handStroke, kStrokeOpen, kStrokeBass);
	l.bendUp = k == Kind::Hand && c.bendDirection == 1;
	l.shell = std::clamp(c.shell, 0.0, 1.0);
	l.pot = std::clamp(c.pot, 0.0, 1.0);
	l.fmRatio = fmRatioEff(std::clamp(c.fmRatio, 0.25, 16.0), std::clamp(c.fmFineCents, -100.0, 100.0), static_cast<double>(f.metal));
	l.fmIndex = std::clamp(c.fmIndex, 0.0, 12.0);
	l.fmIndexT60 = std::clamp(c.fmIndexSeconds, 0.002, 4.0);
	l.fmFeedback = std::clamp(c.fmFeedback, 0.0, 1.0);
	// A DRONE HAS NO STRIKE: the held tone sits at its pitch, no envelope falling onto it.
	if (k == Kind::FM && l.play == kPlayDrone) {
		l.sweepOctaves = 0.0;
	}
	return l;
}

double modeRatio(int k, float metal);
double metalRatio(int i, float metal);

SettledPitch settledPitch(Kind k, const Fields& f, const Controls& c, double baseHz) {
	// THE HZ A HIT'S SWEEP LANDS ON: the laws' own `hz` (the same call lawsFor makes, so the two cannot disagree).
	const double hz = baseHz * std::pow(2.0, terrainPitchSemitones(f.pitch, c) / 12.0);
	switch (k) {
	case Kind::Kick:
	case Kind::String:
		return { true, false, hz };
	case Kind::Particle:
	case Kind::Friction:
	case Kind::Water:
		return { true, true, hz }; // a centre the resonances scatter about, as the Metal's
	case Kind::Membrane:
	case Kind::Snare:
		// The head's lowest mode, which is the pitch heard: mode 0 sits at hz * its ratio at this Metal.
		return { true, false, hz * modeRatio(0, f.metal) };
	case Kind::Hand: return { true, false, hz * handRatio(0, f.metal) };
	case Kind::FM: return { true, false, hz }; // the carrier's pitch the envelope lands on
	case Kind::Metal: {
		// Inharmonic by design: the lowest of its oscillators, offered as a centre rather than a pitch.
		double lowest = metalRatio(0, f.metal);
		for (int i = 1; i < kMetalOscillators; ++i) {
			lowest = std::min(lowest, metalRatio(i, f.metal));
		}
		return { true, true, hz * lowest };
	}
	default: return {};
	}
}

double restingBaseHz(Kind k, double tuneSemitones, double transposeSemitones) {
	return kindBaseHz(k) * std::exp2((tuneSemitones + transposeSemitones) / 12.0);
}

double modeRatio(int k, float metal) {
	// THE STRUCTURE MORPH (doc §13 item 5): Metal walks the ratios through four stations, geometrically between each pair:
	// the membrane's Bessel ratios at 0, a clamped plate's at 1/3, a free bar's at 2/3, the bar stretched further at 1.
	const auto m = static_cast<double>(std::clamp(metal, 0.0f, 1.0f)) * 3.0;
	const auto i = static_cast<size_t>(k);
	const std::array<const std::array<double, kMembraneModes>*, 4> stations { &kMembraneRatios, &kPlateRatios, &kBarRatios,
		                                                                      &kStretchedRatios };
	const size_t s = std::min<size_t>(static_cast<size_t>(m), 2);
	const double u = m - static_cast<double>(s);
	return std::pow((*stations[s])[i], 1.0 - u) * std::pow((*stations[s + 1])[i], u);
}

double fmSideband(int n, double index) {
	const int order = n < 0 ? -n : n;
	const double half = 0.5 * index;
	double term = std::pow(half, order);
	for (int k = 1; k <= order; ++k) {
		term /= static_cast<double>(k);
	}
	double sum = 0.0;
	for (int k = 0; k < 60; ++k) {
		sum += term;
		term *= -half * half / (static_cast<double>(k + 1) * static_cast<double>(k + 1 + order));
	}
	return std::abs(sum);
}

double handRatio(int mode, float metal) {
	const double u = static_cast<double>(std::clamp(metal, 0.0f, 1.0f));
	const double harmonic = static_cast<double>(mode + 1);
	return std::pow(harmonic, 1.0 - u) * std::pow(kMembraneRatios[static_cast<size_t>(mode)], u);
}

double metalRatio(int i, float metal) {
	return std::pow(kMetalRatios[static_cast<size_t>(i)], 0.6 + 0.8 * static_cast<double>(std::clamp(metal, 0.0f, 1.0f)));
}

double modeT60(double t60, double ratio, float metal) { return t60 * std::pow(ratio, -materialExponent(metal)); }

double materialExponent(float metal) { return 0.6 + 1.4 * static_cast<double>(std::clamp(metal, 0.0f, 1.0f)); }

double wireCentreHz(float tone, double hz) { return 16.0 * hz * std::exp2(2.0 * (static_cast<double>(tone) - 0.5)); }
double metalCentreHz(float tone) { return 2000.0 * std::pow(7.0, static_cast<double>(tone)); }
double clapCentreHz(float tone) { return 600.0 * std::pow(5.0, static_cast<double>(tone)); }


double malletWeight(double x) {
	// The raised-cosine (Hann) pulse's spectrum, normalised to 1 at DC: sinc(x) / (1 − x²), with its removable point at 1.
	const double ax = std::abs(x);
	if (ax < 1.0e-9) {
		return 1.0;
	}
	if (std::abs(ax - 1.0) < 1.0e-6) {
		return 0.5;
	}
	const double sinc = std::sin(std::numbers::pi * ax) / (std::numbers::pi * ax);
	return std::abs(sinc / (1.0 - ax * ax));
}
double driveTransfer(int curve, double drive, double x) {
	if (drive <= 0.0) {
		return x;
	}
	const double g = 1.0 + 9.0 * drive;
	switch (curve) {
	case 1: {
		const double b = 0.35; // kTubeBias, in double
		return (std::tanh(g * x + b) - std::tanh(b)) / (std::tanh(g + b) - std::tanh(b));
	}
	case 2: {
		const double u = std::clamp(g * x, -1.0, 1.0);
		return 1.5 * u - 0.5 * u * u * u;
	}
	default: return std::tanh(g * x) / std::tanh(g); // the true curve: the voice's fasttanh stops at ±4.5, 2.5e-4 short of it
	}
}

std::array<int, kMaxClapBursts> clapOnsets(int count, double spreadSeconds, double sampleRate, uint64_t seed) {
	std::array<int, kMaxClapBursts> on {};
	on.fill(-1);
	const int n = std::clamp(count, 1, kMaxClapBursts);
	ks::PluckNoise rng(seed ^ kClapSalt);
	on[0] = 0;
	for (int i = 1; i < n; ++i) {
		const double base = static_cast<double>(i) * spreadSeconds / static_cast<double>(n - 1);
		const double u = 0.5 * static_cast<double>(rng.next());
		const int at = static_cast<int>(std::lround(base * (1.0 + 0.3 * u) * sampleRate));
		on[static_cast<size_t>(i)] = std::max(at, on[static_cast<size_t>(i - 1)] + 1);
	}
	return on;
}

void Env::set(float value, double t60Seconds, double sampleRate) {
	v = value < kSilenceFloor ? 0.0f : value;
	d = envDecay(t60Seconds, sampleRate);
}

void Svf::setBandPass(double hz, double q, double sampleRate) {
	const double fc = std::clamp(hz, 10.0, kTopFraction * sampleRate);
	const double gg = std::tan(std::numbers::pi * fc / sampleRate);
	const double kk = 1.0 / std::max(0.05, q);
	const double b1 = 1.0 / (1.0 + gg * (gg + kk));
	g = static_cast<float>(gg);
	k = static_cast<float>(kk);
	a1 = static_cast<float>(b1);
	a2 = static_cast<float>(gg * b1);
	a3 = static_cast<float>(gg * gg * b1);
}

void Hit::prepare(double sampleRate) {
	sr_ = sampleRate;
	reset();
}

void Hit::reset() {
	idle_ = true;
	controlValid_ = false;
	ratioMetal_ = -1.0f;
	ratioPot_ = -1.0;
	decayT60_ = -1.0;
	handBend_ = 0.0;
	fmPhaseC_ = 0.0;
	fmPhaseM_ = 0.0;
	fmIncC_ = 0.0;
	fmIncM_ = 0.0;
	fmPrev_ = 0.0f;
	fmIndexNow_ = 0.0;
	fmToneScale_ = 1.0;
	fmIdxEnv_ = {};
	body_ = {};
	grain_ = {};
	for (Svf& r : pres_) {
		r.clear();
	}
	for (Svf& r : fres_) {
		r.clear();
	}
	for (WaterChirp& c : chirps_) {
		c = {};
	}
	splashEnv_ = {};
	splashLp_ = 0.0f;
	flowBp_.clear();
	flowBpR_.clear();
	flowWobble_ = 0.0f;
	sweepFilter_.clear();
	sweepPos_ = 0.0;
	sweepEnded_ = false;
	sweepShape_ = 0.0f;
	sweepShapeStep_ = 0.0f;
	fricLp_ = 0.0f;
	slipAmp_ = 0.0f;
	partAcc_ = 0.0f;
	partLp_ = 0.0f;
	partRaw_ = 0.0f;
	click_ = {};
	band_ = {};
	tail_ = {};
	phase_ = 0.0;
	inc_ = 0.0;
	zr_.fill(0.0f);
	zi_.fill(0.0f);
	cr_.fill(0.0f);
	ci_.fill(0.0f);
	mphase_.fill(0.0);
	hpState_ = 0.0f;
	clickLp_ = 0.0f;
	bp_.clear();
	bell_.clear();
	bell2_.clear();
	bodyGated_ = false;
	wireGated_ = false;
	nextBurst_ = 0;
	toothK_ = 0;
	toothInv_ = 0.0f;
	toothValue_ = 0.0f;
	wireLin_ = 0.0f;
	wireLinStep_ = 0.0f;
	modPhase_ = 0.0f;
	buzzFollow_ = 0.0f;
	buzzUsed_ = 1.0f;
	widthOn_ = false;
	bpR_.clear();
	side_ = 0.0f;
	wireEnvUsed_ = 0.0f;
	clapHpLpR_ = 0.0f;
	driveLpR_ = 0.0f;
	toothOn_ = false;
	clapHpLp_ = 0.0f;
	driveLp_ = 0.0f;
	bellOn_ = false;
	bell2On_ = false;
	bell2Used_ = 0.0f;
	bell2Env_ = {};
	onsets_.fill(-1);
	controlLeft_ = 0;
	released_ = false;
	if (kind_ == Kind::String && string_ != nullptr && !string_->idle()) {
		string_->reset();
	}
	stringHz_ = -1.0;
}

void Hit::strike(Kind k, const Laws& l, uint64_t seed, float level) {
	reset();
	kind_ = k;
	seed_ = seed;
	age_ = 0;
	idle_ = false;
	noise_ = ks::PluckNoise(seed);
	ks::PluckNoise vary(seed ^ kVariationSalt);
	toneVariation_ = 1.0 + kToneVariation * static_cast<double>(vary.next());
	ks::PluckNoise varyTension(seed ^ kPhaseSalt ^ 0x7E45105ull);
	tensionVariation_ = 1.0 + kTensionVariation * static_cast<double>(varyTension.next());
	level_ = level * static_cast<float>(std::pow(10.0, kLevelVariationDb * static_cast<double>(vary.next()) / 20.0));
	laws_ = l;
	laws_.tone = clamp01(static_cast<double>(l.tone) * toneVariation_);
	laws_.sweepOctaves = l.sweepOctaves * (kindControls(kind_).tension ? tensionVariation_ : 1.0);
	const Laws& w = laws_;
	switch (k) {
	case Kind::Kick:
		phase_ = 0.0;
		// THE START PHASE (proposal §12 item 3): 0 starts on a zero crossing, 90° on the crest.
		kc_ = static_cast<float>(std::cos(w.startPhase));
		ks_ = static_cast<float>(std::sin(w.startPhase));
		dipGain_ = 1.0f;
		dipStep_ = 0.0f;
		body_.set(level_, w.t60, sr_);
		click_.set(level_ * 0.8f * w.snap, 0.004 + 0.012 * (1.0 - static_cast<double>(w.snap)), sr_);
		band_.set(level_ * 0.25f * w.noise, 0.5 * w.t60, sr_);
		clickCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * (2000.0 * std::pow(8.0, static_cast<double>(w.snap))) / sr_));
		break;
	case Kind::Membrane:
	case Kind::Snare: {
		modeCount_ = k == Kind::Membrane ? kMembraneModes : kSnareHeadModes;
		// THE STRIKE POSITION (Tone): mode k starts at ratio_k^−(2.2 − 2·tone), normalised to sum 1 — a centre strike (low
		// Tone) is nearly the fundamental alone, an edge strike (high Tone) rings the upper modes. Im(z) starts at 0, so the
		// bank's first sample is 0 and a strike never clicks by itself.
		double sum = 0.0;
		std::array<double, kMembraneModes> a {};
		for (int i = 0; i < modeCount_; ++i) {
			a[static_cast<size_t>(i)] = std::pow(modeRatio(i, w.metal), -(2.2 - 2.0 * static_cast<double>(w.tone)));
			sum += a[static_cast<size_t>(i)];
		}
		const double head = k == Kind::Snare ? 1.0 - 0.6 * static_cast<double>(w.noise) : 1.0;
		// MALLET STIFFNESS (doc §13 item 3): the strike is a raised-cosine pulse of the contact time, not an impulse, so mode k
		// is weighted by that pulse's spectrum at its frequency at the strike (the sweep's top): |sinc(x) / (1 − x²)|, x = f·Tc
		// (1 at DC, a half at x = 1, nothing at x = 2). A soft, slow mallet leaves the upper modes unstruck.
		// MICRO-VARIATION (doc §13 item 10): each mode starts at a small seeded phase, ±0.1 rad, so no two hits' modes line up
		// alike; the first sample stays near 0 (at most a tenth of the level), so a strike still does not click by itself.
		ks::PluckNoise phases(seed ^ kPhaseSalt);
		const double strikeHz = w.hz * std::exp2(w.sweepOctaves);
		for (int i = 0; i < modeCount_; ++i) {
			const auto u = static_cast<size_t>(i);
			const double mallet = malletWeight(strikeHz * modeRatio(i, w.metal) * w.malletSeconds);
			const double amp = static_cast<double>(level_) * head * a[u] / sum * mallet;
			const double phi = kModePhaseJitter * static_cast<double>(phases.next());
			zr_[u] = static_cast<float>(amp * std::cos(phi));
			zi_[u] = static_cast<float>(amp * std::sin(phi));
		}
		click_.set(level_ * 0.5f * w.snap, 0.003 + 0.010 * (1.0 - static_cast<double>(w.snap)), sr_);
		clickCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * (1000.0 * std::pow(12.0, static_cast<double>(w.snap))) / sr_));
		if (k == Kind::Snare) {
			// THE SNARE'S THREE ENVELOPES (Requirement: essential): the head's by Decay, the wires' by Wire Decay, the click's by Click.
			band_.set(level_ * (0.3f + 0.7f * w.noise), w.wireT60, sr_);
			click_.set(level_ * 0.5f * w.snap, w.clickT60, sr_);
			bell_.clear();
			bell2_.clear();
			bell2Env_.set(1.0f, w.bell2T60, sr_);
			bell2Used_ = 0.0f;
			// THE WIRES' CONTOUR: Linear starts where the exponential does and falls to zero in the wires' T60.
			wireLin_ = band_.v;
			wireLinStep_ = static_cast<float>(static_cast<double>(band_.v) / (w.wireT60 * sr_));
			modPhase_ = 0.0f;
			buzzFollow_ = 0.0f;
			noiseR_ = ks::PluckNoise(seed ^ kWidthSalt);
		}
		else {
			band_.set(level_ * 0.5f * w.noise, 0.3 * w.t60, sr_);
		}
		break;
	}
	case Kind::Metal:
		mphaseF_.fill(0.0f);
		mphase_.fill(0.0);
		body_.set(level_, w.t60, sr_);
		click_.set(level_ * 2.0f * w.snap, 0.01, sr_);
		break;
	case Kind::Clap:
		onsets_ = clapOnsets(w.count, w.spreadSeconds, sr_, seed);
		noiseR_ = ks::PluckNoise(seed ^ kWidthSalt);
		nextBurst_ = 0;
		break;
	case Kind::String:
		if (string_ == nullptr || builder_ == nullptr) {
			idle_ = true;
			return;
		}
		string_->reset();
		break;
	case Kind::Particle: {
		// THE SHOWER'S OWN STREAM from the seed (never a count or a clock), so a reload and a retrigger from one seed are one sound. The
		// density envelope starts at full: One-shot lets it decay by Decay, Gate holds it until released, Drone never lets it go.
		partRand_ = ks::PluckNoise(seed ^ kParticleSalt);
		body_.v = 1.0f;
		body_.d = 1.0f;
		grain_ = {};
		grain_.d = envDecay(w.grainT60, sr_);
		partLp_ = 0.0f;
		partRaw_ = 0.0f;
		grains_ = 0;
		const auto unit = [&] { return std::clamp(0.5 * (static_cast<double>(partRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6); };
		// THE RESONANCES: the first at the centre, the rest scattered about it by up to 1.5 octaves at full Metal, seeded; each with its own gain.
		for (int i = 0; i < kParticleResonators; ++i) {
			const auto u = static_cast<size_t>(i);
			const double scatter = i == 0 ? 0.0 : (2.0 * unit() - 1.0);
			presRatio_[u] = std::exp2(1.5 * static_cast<double>(w.metal) * scatter);
			presGain_[u] = static_cast<float>(i == 0 ? 1.0 : 0.5 + 0.5 * unit());
			pres_[u].clear();
		}
		// THE FIRST COLLISION: a key (One-shot or Gate) lands one at once; a drone starts where its random process does.
		partThresh_ = static_cast<float>(-std::log(1.0 - unit()));
		partAcc_ = w.play == kPlayDrone ? 0.0f : partThresh_;
		break;
	}
	case Kind::Sweep: {
		sweepNoise_ = ks::PluckNoise(seed ^ kSweepSalt);
		body_.v = 1.0f;
		body_.d = 1.0f;
		sweepPos_ = 0.0;
		sweepStep_ = 1.0 / (std::max(0.005, w.lengthSeconds) * sr_);
		sweepDir_ = 1.0;
		sweepEnded_ = false;
		sweepOscPhase_ = 0.0;
		sweepFilter_.clear();
		sweepShape_ = static_cast<float>(sweepShape(0.0, w.sweepDirection, w.play));
		sweepShapeStep_ = 0.0f;
		break;
	}
	case Kind::Water: {
		partRand_ = ks::PluckNoise(seed ^ kWaterSalt);
		waterNoiseR_ = ks::PluckNoise(seed ^ kWaterSalt ^ kWidthSalt);
		fricRand_ = ks::PluckNoise(seed ^ kWaterSalt ^ 0x77ull); // the bed's swell, its own stream so Flow never moves the drops
		body_.v = 1.0f;
		body_.d = 1.0f;
		for (WaterChirp& c : chirps_) {
			c = {};
		}
		splashEnv_ = {};
		splashLp_ = 0.0f;
		flowBp_.clear();
		flowBpR_.clear();
		flowWobble_ = 0.0f;
		events_ = 0;
		lastStartHz_ = 0.0;
		lastRiseOct_ = 0.0;
		lastPan_ = 0.0f;
		const double u = std::clamp(0.5 * (static_cast<double>(partRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6);
		partThresh_ = static_cast<float>(-std::log(1.0 - u));
		// A KEY LANDS A DROP AT ONCE; a drone starts where its random process does.
		partAcc_ = w.play == kPlayDrone ? 0.0f : partThresh_;
		break;
	}
	case Kind::Friction: {
		fricRand_ = ks::PluckNoise(seed ^ kFrictionSalt);
		body_.v = 1.0f;
		body_.d = 1.0f;
		fricLp_ = 0.0f;
		fricScrape_ = 0.0f;
		slips_ = 0;
		slipAmp_ = 0.0f;
		// THE RESONANCES: the fundamental, a second at an inharmonic ratio (2.1 to 3.4, Metal), a third an octave and a fifth over; each seeded in gain.
		const auto u01 = [&] { return std::clamp(0.5 * (static_cast<double>(fricRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6); };
		fresRatio_ = { 1.0, 2.1 + 1.3 * static_cast<double>(w.metal), 3.0 + 2.0 * static_cast<double>(w.metal) };
		fresGain_ = { 1.0f, static_cast<float>(0.25 + 0.2 * u01()), static_cast<float>(0.1 + 0.1 * u01()) };
		for (Svf& r : fres_) {
			r.clear();
		}
		slipCount_ = 0.0; // the first slip is the first sample: a bow lands
		slipPeriod_ = 1.0;
		break;
	}
	case Kind::Hand: {
		// THE HEAD'S MODES at the strike position (Tone: mode k starts at ratio_k^-(2.2 - 2·tone), as a Membrane's), the stroke's fundamental weight on the lowest, each
		// weighted by the contact's spectrum at its frequency at the strike (the bend's far end), and the shell's one more mode beside them, at its own level.
		modeCount_ = kHandHeadModes + 1;
		const HandStroke stroke = handStroke(w.handStroke);
		const double toneExp = 2.2 - 2.0 * static_cast<double>(w.tone);
		double sum = 0.0;
		std::array<double, kMembraneModes> a {};
		for (int i = 0; i < kHandHeadModes; ++i) {
			a[static_cast<size_t>(i)] = std::pow(handRatio(i, w.metal), -toneExp) * (i == 0 ? stroke.fundamental : 1.0);
			sum += a[static_cast<size_t>(i)];
		}
		ks::PluckNoise phases(seed ^ kPhaseSalt);
		const double strikeHz = w.hz * std::exp2(w.bendUp ? -w.sweepOctaves : w.sweepOctaves);
		for (int i = 0; i < kHandHeadModes; ++i) {
			const auto u = static_cast<size_t>(i);
			const double mallet = malletWeight(strikeHz * handRatio(i, w.metal) * w.malletSeconds);
			const double amp = static_cast<double>(level_) * a[u] / sum * mallet;
			const double phi = kModePhaseJitter * static_cast<double>(phases.next());
			zr_[u] = static_cast<float>(amp * std::cos(phi));
			zi_[u] = static_cast<float>(amp * std::sin(phi));
		}
		const auto shell = static_cast<size_t>(kHandHeadModes);
		const double shellAmp = static_cast<double>(level_) * kHandShellGain * w.shell;
		const double shellPhi = kModePhaseJitter * static_cast<double>(phases.next());
		zr_[shell] = static_cast<float>(shellAmp * std::cos(shellPhi));
		zi_[shell] = static_cast<float>(shellAmp * std::sin(shellPhi));
		// THE HAND'S OWN TAP beside the head (a slap's crack, a muted stroke's slap), and the skin's and fingers' noise.
		click_.set(std::min(1.0f, level_ * 0.5f * w.snap * static_cast<float>(stroke.crack)), 0.003 + 0.010 * (1.0 - static_cast<double>(w.snap)), sr_);
		clickCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * (1000.0 * std::pow(12.0, static_cast<double>(w.snap))) / sr_));
		band_.set(level_ * 0.5f * w.noise, 0.3 * w.t60, sr_);
		break;
	}
	case Kind::FM: {
		// A STRIKE STARTS BOTH PHASES AT ZERO, so the first sample is exactly 0 and a hit never clicks by itself. The index starts at its peak and falls (a Drone
		// has no strike: it sits at its held share). The tap and the noise transient are the strike's, so a Drone has none.
		fmPhaseC_ = 0.0;
		fmPhaseM_ = 0.0;
		fmPrev_ = 0.0f;
		fmIdxEnv_.v = w.play == kPlayDrone ? 0.0f : 1.0f;
		fmIdxEnv_.d = envDecay(w.fmIndexT60, sr_);
		body_.v = 1.0f;
		body_.d = 1.0f;
		const bool struck = w.play != kPlayDrone;
		click_.set(struck ? level_ * 0.3f * w.snap : 0.0f, 0.002 + 0.008 * (1.0 - static_cast<double>(w.snap)), sr_);
		clickCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * (2000.0 * std::pow(8.0, static_cast<double>(w.snap))) / sr_));
		band_.set(struck ? level_ * 0.5f * w.noise : 0.0f, std::min(0.25, 0.15 * w.t60), sr_);
		break;
	}
	default: break;
	}
	control();
	controlLeft_ = kControlBlock;
	if (k == Kind::String) {
		ks::Excitation e;
		e.noise = 1.0f;
		e.hardness = w.snap;
		e.pick = 0.05 + 0.4 * static_cast<double>(w.tone);
		e.seed = seed;
		e.level = level_;
		builder_->pluck(*string_, e);
	}
}

void Hit::setLaws(const Laws& l) {
	const Laws was = laws_;
	laws_ = l;
	laws_.tone = clamp01(static_cast<double>(l.tone) * toneVariation_);
	laws_.sweepOctaves = l.sweepOctaves * (kindControls(kind_).tension ? tensionVariation_ : 1.0);
	if (!(laws_ == was)) {
		controlLeft_ = 0;
	}
}

void Hit::release(double t60Seconds) {
	if (released_ && t60Seconds >= releaseT60_) {
		return;
	}
	released_ = true;
	releaseT60_ = std::max(0.0, t60Seconds);
	const float d = envDecay(releaseT60_, sr_);
	click_.d = std::min(click_.d, d);
	controlLeft_ = 0;
}

double Hit::effective(double t60) const { return released_ ? std::min(t60, releaseT60_) : t60; }

void Hit::setModes(int count, double bend) {
	// A HAND DRUM'S LAST MODE IS ITS SHELL: a cavity's resonance, which does not bend with the head, at its own ratio and its own ring.
	const bool hand = kind_ == Kind::Hand;
	const int heads = hand ? count - 1 : count;
	const double scale = laws_.hz * std::exp2(bend);
	// THE RATIOS AND THE DECAYS ARE CACHED on what they read (Metal and the shell's Pot; the T60 and the Gate): a sweeping hit moves only its
	// frequencies, so only the rotations are computed again while it bends (measured: a burst of struck Membranes at 1024
	// frames cost about twice a resting hit's while every block re-ran sixteen pows for the ratios and sixteen for the decays).
	const double t60 = bodyGated_ ? std::min(effective(laws_.t60), kGateFall) : effective(laws_.t60);
	const bool ratiosStale = !(ratioMetal_ == laws_.metal) || ratioCount_ != count || (hand && !(ratioPot_ == laws_.pot));
	if (ratiosStale) {
		for (int i = 0; i < heads; ++i) {
			ratio_[static_cast<size_t>(i)] = hand ? handRatio(i, laws_.metal) : modeRatio(i, laws_.metal);
		}
		if (hand) {
			ratio_[static_cast<size_t>(heads)] = handShellRatio(laws_.pot);
		}
		ratioMetal_ = laws_.metal;
		ratioPot_ = laws_.pot;
		ratioCount_ = count;
	}
	const bool decaysStale = ratiosStale || !(decayT60_ == t60);
	if (decaysStale) {
		for (int i = 0; i < heads; ++i) {
			// A hand drum's skin keeps the skin's law (Metal's material is a Membrane's: a hand drum is always skin).
			decayR_[static_cast<size_t>(i)] = decayFactor(modeT60(t60, ratio_[static_cast<size_t>(i)], hand ? 0.0f : laws_.metal), sr_);
		}
		if (hand) {
			decayR_[static_cast<size_t>(heads)] = decayFactor(t60 * handShellT60Share(laws_.pot), sr_);
		}
		decayT60_ = t60;
	}
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		const double ratio = ratio_[u];
		const double hz = (hand && i == heads ? laws_.hz : scale) * ratio;
		if (hz >= kTopFraction * sr_) {
			// ABOVE THE TOP the mode is silent, never aliased: its state goes to zero.
			modeHz_[u] = hz;
			modeR_[u] = 0.0;
			zr_[u] = 0.0f;
			zi_[u] = 0.0f;
			cr_[u] = 0.0f;
			ci_[u] = 0.0f;
			continue;
		}
		const double r = decayR_[u];
		const double w = kTwoPi * hz / sr_;
		modeHz_[u] = hz;
		modeR_[u] = r;
		cr_[u] = static_cast<float>(r * std::cos(w));
		ci_[u] = static_cast<float>(r * std::sin(w));
	}
}

double Hit::dipAt(double t) const {
	// THE FISHTAIL (proposal §12 item 1): a raised-cosine dip of depth Dip, deepest at Dip Time and spanning half of it either side,
	// so the snap, the dip and the boom read as three. Exactly 1 outside its window.
	if (kind_ != Kind::Kick || laws_.kickDip <= 0.0) {
		return 1.0;
	}
	const double centre = laws_.kickDipTime;
	const double u = (t - 0.5 * centre) / centre;
	if (u <= 0.0 || u >= 1.0) {
		return 1.0;
	}
	return 1.0 - laws_.kickDip * 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * u));
}

void Hit::control() {
	const double t = static_cast<double>(age_) / sr_;
	// THE SWEEP ENDS: past 40 time constants (or below a tenth of a cent, below) it is below 1e-17 of its depth, and is taken as exactly none, so the key below stops
	// moving and a resting hit pays for no control block at all.
	const double x = laws_.sweepTau > 0.0 ? t / laws_.sweepTau : 1.0e9;
	double bend = laws_.sweepOctaves > 0.0 && x < 40.0 ? laws_.sweepOctaves * std::exp(-x) : 0.0;
	// BELOW A TENTH OF A CENT THE SWEEP HAS LANDED: taken as none from there, so the rotations stop being recomputed.
	if (bend < 1.0e-4) {
		bend = 0.0;
	}
	// A TENSION GLIDE MOVES IN HALF-CENT STEPS (measured 2026-11-06: following the ring, a Membrane's glide lasts as long as it
	// rings, and re-tuning sixteen modes every control block took it from 2.6 to 6.3 µs a further voice). On a Membrane or a
	// Snare the bend is rounded to 1/2400 of an octave, so the modes are re-tuned only when it has moved half a cent, which
	// no ear follows; the fast early glide still re-tunes nearly every block.
	if (kind_ == Kind::Membrane || kind_ == Kind::Snare || kind_ == Kind::Hand) {
		bend = std::round(bend * kTensionSteps) / kTensionSteps;
	}
	// THE HAND DRUM'S BEND MAY RISE: it starts under the pitch and lands on it, so the bend is signed.
	if (kind_ == Kind::Hand && laws_.bendUp) {
		bend = -bend;
	}
	// THE SNARE'S MOVING PARTS: the wires' filter bend and the bell's centre, each landed (taken as still) once within a tenth
	// of a cent of rest, and its gates, so a resting snare pays for no control block either.
	double wireBend = 0.0;
	double bellHz = laws_.bellEndHz;
	bool gates[2] = { false, false };
	if (kind_ == Kind::Snare) {
		const double wx = t / std::max(1.0e-4, laws_.wireT60 / 6.0);
		wireBend = laws_.wireSweepOctaves != 0.0 && wx < 40.0 ? laws_.wireSweepOctaves * std::exp(-wx) : 0.0;
		if (std::abs(wireBend) < 1.0e-4) {
			wireBend = 0.0;
		}
		const double bx = t / laws_.bellTau;
		const double span = std::log2(laws_.bellStartHz / laws_.bellEndHz);
		const double bellBend = bx < 40.0 ? span * std::exp(-bx) : 0.0;
		bellHz = laws_.bellEndHz * (std::abs(bellBend) < 1.0e-4 ? 1.0 : std::exp2(bellBend));
		gates[0] = laws_.bodyGate > 0.0 && t >= laws_.bodyGate;
		gates[1] = laws_.wireGate > 0.0 && t >= laws_.wireGate;
	}
	// THE KICK'S SHAPE: the click sweep rides on top of Sweep (landed below a tenth of a cent); Hold keeps the body at full;
	// the fishtail's gain moves only inside its window, so a resting kick still pays for no control block.
	bool holding = false;
	double dip = 1.0;
	if (kind_ == Kind::Kick) {
		const double cx = t / laws_.clickSweepTau;
		const double click = laws_.clickSweepOctaves > 0.0 && cx < 40.0 ? laws_.clickSweepOctaves * std::exp(-cx) : 0.0;
		bend += click >= 1.0e-4 ? click : 0.0;
		holding = t < laws_.kickHold;
		dip = dipAt(t);
	}
	// NOTHING MOVED, NOTHING TO DO (measured 2026-11-05: recomputing every mode's ratio, decay and rotation each block cost a
	// Membrane hit about 13 µs a 256-frame block, V5's price). The laws, the bend and the Gate are everything a block reads.
	const ControlKey key { laws_, bend, released_, releaseT60_, wireBend, bellHz, gates[0], gates[1], holding, dip, sweepPos_ };
	if (controlValid_ && key == lastControl_) {
		return;
	}
	lastControl_ = key;
	controlValid_ = true;
	// THE DRIVE, ON EVERY KIND (proposal §12 item 4): Drive sets the gain into the curve, 1 + 9·Drive; each
	// curve is normalised so a full-scale input still comes out at full scale, and 0 in is exactly 0 out (a tail still ends at
	// exact zero). Drive Tone tilts what goes in, about a 600 Hz pole: the middle is flat and skipped.
	driveCurve_ = laws_.driveCurve;
	if (laws_.drive > 0.0) {
		const double g = 1.0 + 9.0 * laws_.drive;
		driveGain_ = static_cast<float>(g);
		switch (driveCurve_) {
		case 1: {
			driveBias_ = kTubeBias;
			driveOffset_ = dsp::fasttanh(kTubeBias);
			driveNorm_ = static_cast<float>(1.0 / (std::tanh(g + static_cast<double>(kTubeBias)) - static_cast<double>(driveOffset_)));
			break;
		}
		case 2: driveNorm_ = 1.0f; break; // the cubic reaches 1 at its knee, and g·1 ≥ 1 is past it
		default: driveNorm_ = static_cast<float>(1.0 / std::tanh(g)); break;
		}
	}
	else {
		driveGain_ = 0.0f;
		driveNorm_ = 1.0f;
	}
	driveTilt_ = static_cast<float>(2.0 * laws_.driveTone - 1.0);
	driveLpCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * kDriveTiltHz / sr_));
	switch (kind_) {
	case Kind::Kick:
		inc_ = laws_.hz * std::exp2(bend) / sr_;
		rotC_ = static_cast<float>(std::cos(kTwoPi * inc_));
		rotS_ = static_cast<float>(std::sin(kTwoPi * inc_));
		body_.d = envDecay(effective(laws_.t60), sr_);
		// HOLD: the body at full until Hold has passed (its decay factor 1), then its own decay.
		holding_ = holding;
		if (holding && !released_) {
			body_.d = 1.0f;
		}
		// THE FISHTAIL: this block's gain, ramped to the next block's over the block.
		dipGain_ = static_cast<float>(dip);
		dipStep_ = static_cast<float>((dipAt(t + kControlBlock / sr_) - dip) / kControlBlock);
		band_.d = envDecay(effective(0.5 * laws_.t60), sr_);
		break;
	case Kind::Membrane:
		setModes(modeCount_, bend);
		band_.d = envDecay(effective(0.3 * laws_.t60), sr_);
		bandHz_ = std::min(2.0 * laws_.hz, kTopFraction * sr_);
		bp_.setBandPass(bandHz_, 1.0, sr_);
		break;
	case Kind::Hand:
		handBend_ = bend;
		setModes(modeCount_, bend);
		band_.d = envDecay(effective(0.3 * laws_.t60), sr_);
		bandHz_ = std::min(2.0 * laws_.hz, kTopFraction * sr_);
		bp_.setBandPass(bandHz_, 1.0, sr_);
		break;
	case Kind::FM: {
		// THE CARRIER FALLS ONTO ITS PITCH by the envelope (a Drone has none); the modulator keeps its ratio to it. Neither may pass the top.
		const double carrier = std::min(laws_.hz * std::exp2(bend), kTopFraction * sr_);
		fmIncC_ = carrier / sr_;
		fmIncM_ = std::min(carrier * laws_.fmRatio, kTopFraction * sr_) / sr_;
		fmIdxEnv_.d = envDecay(laws_.fmIndexT60, sr_);
		fmToneScale_ = fmToneScale(static_cast<double>(laws_.tone));
		// THE AMPLITUDE: One-shot rings out by Decay; Gate and Drone hold it, and a released Gate falls by Release.
		if (laws_.play == kPlayOneShot) {
			body_.d = envDecay(effective(laws_.t60), sr_);
		}
		else {
			body_.d = released_ ? envDecay(releaseT60_, sr_) : 1.0f;
		}
		band_.d = envDecay(effective(std::min(0.25, 0.15 * laws_.t60)), sr_);
		bandHz_ = std::min(4.0 * laws_.hz, kTopFraction * sr_);
		bp_.setBandPass(bandHz_, 1.0, sr_);
		break;
	}
	case Kind::Snare: {
		// THE GATES: past Shape · T60 a component falls in kGateFall (the body through its modes, the wires through their band).
		bodyGated_ = laws_.bodyGate > 0.0 && t >= laws_.bodyGate;
		wireGated_ = laws_.wireGate > 0.0 && t >= laws_.wireGate;
		setModes(modeCount_, bend);
		const double wt60 = wireGated_ ? std::min(laws_.wireT60, kGateFall) : laws_.wireT60;
		band_.d = envDecay(effective(wt60), sr_);
		// THE WIRES' FILTER ENVELOPE: the band starts Wire Sweep octaves off its centre and settles over the wires' T60 / 6.
		bandHz_ = std::min(wireCentreHz(laws_.tone, laws_.hz) * std::exp2(wireBend), kTopFraction * sr_);
		bp_.setBandPass(bandHz_, 0.9, sr_);
		// THE WIRES' CONTOUR, BUZZ AND WIDTH (doc §13 items 7 and 9).
		contour_ = laws_.wireContour;
		modInc_ = static_cast<float>(kWireModHz / sr_);
		if (contour_ == 1) {
			// Linear: past the gate it falls in kGateFall, and a release only ever shortens it, as the exponential's.
			const double rest = wireGated_ ? kGateFall : (released_ ? std::min(laws_.wireT60, releaseT60_) : laws_.wireT60);
			wireLinStep_ = std::max(wireLinStep_, static_cast<float>(static_cast<double>(wireLin_) / (rest * sr_)));
		}
		buzzAmount_ = static_cast<float>(laws_.buzz);
		buzzCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (kBuzzFollowSeconds * sr_)));
		buzzKnee_ = static_cast<float>(kBuzzFullSwing * laws_.buzzThreshold);
		buzzSpan_ = static_cast<float>(std::max(1.0e-3, kBuzzFullSwing * (1.0 - laws_.buzzThreshold)));
		widthOn_ = laws_.width > 0.0;
		widthC_ = static_cast<float>(std::cos(0.5 * std::numbers::pi * laws_.width));
		widthS_ = static_cast<float>(std::sin(0.5 * std::numbers::pi * laws_.width));
		if (widthOn_) {
			bpR_.setBandPass(bandHz_, 0.9, sr_);
		}
		// THE MOVING BELL: a resonant band-pass gliding from Bell Start to Bell End, 95% there by Bell Time.
		bellHz_ = std::min(bellHz, kTopFraction * sr_);
		if (laws_.bellLevel > 0.0) {
			bell_.setBandPass(bellHz_, laws_.bellQ, sr_);
		}
		bellOn_ = laws_.bellLevel > 0.0;
		bellGain_ = static_cast<float>(4.0 * laws_.bellLevel);
		// THE SECOND BELL: Linked, Ratio × the first bell's centre now (so it rides the glide); Free, its own Hz.
		bell2Hz_ = std::min(laws_.bell2Linked ? bellHz * laws_.bell2Ratio : laws_.bell2Hz, kTopFraction * sr_);
		bell2On_ = laws_.bell2Level > 0.0;
		if (bell2On_) {
			bell2_.setBandPass(bell2Hz_, laws_.bell2Q, sr_);
		}
		bell2Gain_ = static_cast<float>(4.0 * laws_.bell2Level);
		break;
	}
	case Kind::Metal: {
		for (int i = 0; i < kMetalOscillators; ++i) {
			minc_[static_cast<size_t>(i)] = std::min(laws_.hz * metalRatio(i, laws_.metal), kTopFraction * sr_) / sr_;
			mincF_[static_cast<size_t>(i)] = static_cast<float>(minc_[static_cast<size_t>(i)]);
			invDtF_[static_cast<size_t>(i)] = 1.0f / mincF_[static_cast<size_t>(i)];
			laneW_[static_cast<size_t>(i)] = 1.0f / static_cast<float>(kMetalOscillators);
		}
		for (size_t i = kMetalOscillators; i < kLanes; ++i) {
			mincF_[i] = 0.25f; // a silent lane: any increment, weight 0
			invDtF_[i] = 4.0f;
			laneW_[i] = 0.0f;
		}
		body_.d = envDecay(effective(laws_.t60), sr_);
		bandHz_ = std::min(metalCentreHz(laws_.tone), kTopFraction * sr_);
		bp_.setBandPass(bandHz_, 1.2, sr_);
		hpCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * 0.5 * bandHz_ / sr_));
		break;
	}
	case Kind::Clap:
		bandHz_ = std::min(clapCentreHz(laws_.tone) * laws_.hz / kindBaseHz(Kind::Clap), kTopFraction * sr_);
		// THE CLAP'S FILTER (the clap decision): its band-pass Q, and a one-pole high-pass after it (at 20 Hz, nothing taken).
		bp_.setBandPass(bandHz_, laws_.clapQ, sr_);
		clapHpCoef_ = laws_.clapHighPassHz > 20.0 ? static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(laws_.clapHighPassHz, kTopFraction * sr_) / sr_)) : 0.0f;
		clapGain_ = static_cast<float>(3.0 * std::sqrt(1.6 / laws_.clapQ));
		// WIDTH: the right ear's band is cos·(the left's) + sin·(its own, from a second noise), θ = Width·π/2.
		widthOn_ = laws_.width > 0.0;
		widthC_ = static_cast<float>(std::cos(0.5 * std::numbers::pi * laws_.width));
		widthS_ = static_cast<float>(std::sin(0.5 * std::numbers::pi * laws_.width));
		if (widthOn_) {
			bpR_.setBandPass(bandHz_, laws_.clapQ, sr_);
		}
		if (tail_.v > 0.0f) {
			tail_.d = envDecay(effective(laws_.t60), sr_);
		}
		if (released_) {
			body_.d = std::min(body_.d, envDecay(releaseT60_, sr_));
		}
		break;
	case Kind::Particle: {
		// THE DENSITY ENVELOPE: One-shot decays by Decay (shortened by a release); Gate and Drone hold it, and a released Gate falls by Release.
		if (laws_.play == kPlayOneShot) {
			body_.d = envDecay(effective(laws_.t60), sr_);
		}
		else {
			body_.d = released_ ? envDecay(releaseT60_, sr_) : 1.0f;
		}
		grain_.d = envDecay(laws_.grainT60, sr_);
		// THE EXCITATION'S BRIGHTNESS: a one-pole low-pass from 400 Hz (dark) to 12.8 kHz (bright) across Tone.
		const double lpHz = std::min(400.0 * std::exp2(5.0 * static_cast<double>(laws_.tone)), kTopFraction * sr_);
		partLpCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * lpHz / sr_));
		// THE RESONANCES ring for a fraction of the shower's own length; each is normalised to unity at its centre, and given back what the
		// narrower band takes from a noise burst (the square root of its Q).
		const double ring = std::clamp(0.12 * laws_.t60, 0.003, 2.0);
		for (int i = 0; i < kParticleResonators; ++i) {
			const auto u = static_cast<size_t>(i);
			const double hz = std::min(laws_.hz * presRatio_[u], kTopFraction * sr_);
			const double q = std::clamp(std::numbers::pi * hz * ring / std::log(1000.0), 1.5, 400.0);
			pres_[u].setBandPass(hz, q, sr_);
			presQGain_[u] = static_cast<float>(pres_[u].k * std::sqrt(q));
		}
		break;
	}
	case Kind::Sweep: {
		const double length = std::max(0.005, laws_.lengthSeconds);
		sweepStep_ = 1.0 / (length * sr_);
		if (laws_.play == kPlayGate && released_) {
			body_.d = envDecay(releaseT60_, sr_);
		}
		const double lo = std::clamp(laws_.hz, 20.0, kTopFraction * sr_);
		const double frac = laws_.sweepDirection == 0 ? sweepPos_ : 1.0 - sweepPos_;
		sweepCorner_ = std::min(sweepCornerHz(lo, laws_.sweepRange, laws_.sweepDirection, sweepPos_), kTopFraction * sr_);
		const double follow = static_cast<double>(laws_.sweepOctaves);
		sweepOscInc_ = std::min(lo * std::exp2(laws_.sweepRange * follow * frac), kTopFraction * sr_) / sr_;
		// THE RESONANCE (Snap): Q from 0.7 (open) to 12 (a whistle); Tone blends the low-pass (dark) into the band-pass (a narrow, vocal sweep).
		const double q = 0.7 + 11.3 * static_cast<double>(laws_.snap) * static_cast<double>(laws_.snap);
		sweepFilter_.setBandPass(sweepCorner_, q, sr_);
		sweepLowMix_ = 1.0f - laws_.tone;
		sweepBandMix_ = laws_.tone * sweepFilter_.k;
		sweepGain_ = static_cast<float>(1.4 / std::sqrt(q));
		// THE LOUDNESS SHAPE, ramped across this block to the next block's.
		const double ahead = laws_.play == kPlayDrone ? sweepPos_ : std::min(1.0, sweepPos_ + sweepStep_ * kControlBlock);
		sweepShape_ = static_cast<float>(sweepShape(sweepPos_, laws_.sweepDirection, laws_.play));
		sweepShapeStep_ = static_cast<float>((sweepShape(ahead, laws_.sweepDirection, laws_.play) - static_cast<double>(sweepShape_)) / kControlBlock);
		break;
	}
	case Kind::Water: {
		if (laws_.play == kPlayOneShot) {
			body_.d = envDecay(effective(laws_.t60), sr_);
		}
		else {
			body_.d = released_ ? envDecay(releaseT60_, sr_) : 1.0f;
		}
		// THE SPLASH'S AND THE BED'S COLOUR: Tone moves the splash's low-pass from 1.5 to 16 kHz; the bed's band centres on the size's pitch,
		// brighter with Tone, so the wash follows what the drops are doing.
		const double lpHz = std::min(1500.0 * std::exp2(3.4 * static_cast<double>(laws_.tone)), kTopFraction * sr_);
		splashLpCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * lpHz / sr_));
		splashEnv_.d = envDecay(0.004 + 0.012 * (1.0 - static_cast<double>(laws_.snap)), sr_);
		const double bedHz = std::clamp(laws_.hz * (0.5 + 1.5 * static_cast<double>(laws_.tone)), 60.0, kTopFraction * sr_);
		flowBp_.setBandPass(bedHz, 0.7, sr_);
		flowBpR_.setBandPass(bedHz, 0.7, sr_);
		flowWobbleCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * 3.0 / sr_));
		flowGain_ = static_cast<float>(0.55 * laws_.flow * std::sqrt(0.7));
		break;
	}
	case Kind::Friction: {
		if (laws_.play == kPlayOneShot) {
			body_.d = envDecay(effective(laws_.t60), sr_);
		}
		else {
			body_.d = released_ ? envDecay(releaseT60_, sr_) : 1.0f;
		}
		// THE SLIP'S BRIGHTNESS: a one-pole low-pass on the impulse, 800 Hz (dark) to 20 kHz across Tone and Pressure (a harder press lets go sharper).
		const double lpHz = std::min(800.0 * std::exp2(4.6 * (0.5 * static_cast<double>(laws_.tone) + 0.3 * laws_.pressure + 0.2 * static_cast<double>(laws_.snap))), kTopFraction * sr_);
		fricLpCoef_ = static_cast<float>(1.0 - std::exp(-kTwoPi * lpHz / sr_));
		const double ring = std::clamp(0.1 * laws_.t60, 0.01, 1.5);
		for (int i = 0; i < kFrictionResonators; ++i) {
			const auto u = static_cast<size_t>(i);
			const double hz = std::min(laws_.hz * fresRatio_[u], kTopFraction * sr_);
			const double q = std::clamp(std::numbers::pi * hz * ring / std::log(1000.0), 2.0, 600.0);
			fres_[u].setBandPass(hz, q, sr_);
			fresQGain_[u] = static_cast<float>(fres_[u].k * std::sqrt(q));
		}
		break;
	}
	case Kind::String:
		if (string_ != nullptr) {
			const double hz = laws_.hz * std::exp2(bend);
			if (hz != stringHz_) {
				string_->setFrequency(hz);
				stringHz_ = hz;
			}
			string_->setDamping(0.25 * (1.0 - static_cast<double>(laws_.tone)) * (1.0 - static_cast<double>(laws_.tone)));
			string_->setDispersion(-0.9 * static_cast<double>(laws_.metal));
			string_->setDecay(effective(laws_.t60), age_ == 0 ? 0 : kControlBlock);
			string_->setPickups(0.13, 0.13);
		}
		break;
	default: break;
	}
}

bool Hit::envelopesSilent() const {
	switch (kind_) {
	case Kind::Kick: return body_.v == 0.0f && click_.v == 0.0f && band_.v == 0.0f;
	case Kind::FM: return body_.v == 0.0f && click_.v == 0.0f && band_.v == 0.0f;
	case Kind::Membrane:
	case Kind::Hand:
	case Kind::Snare: {
		for (int i = 0; i < modeCount_; ++i) {
			if (zr_[static_cast<size_t>(i)] != 0.0f || zi_[static_cast<size_t>(i)] != 0.0f) {
				return false;
			}
		}
		return click_.v == 0.0f && band_.v == 0.0f;
	}
	case Kind::Metal: return body_.v == 0.0f && click_.v == 0.0f;
	case Kind::Clap: return nextBurst_ >= laws_.count && !toothOn_ && tail_.v == 0.0f;
	case Kind::String: return string_ == nullptr || string_->idle();
	case Kind::Sweep: return body_.v == 0.0f && sweepFilter_.ic1 == 0.0f && sweepFilter_.ic2 == 0.0f;
	case Kind::Water:
		for (const WaterChirp& c : chirps_) {
			if (c.on) {
				return false;
			}
		}
		return body_.v == 0.0f && splashEnv_.v == 0.0f && splashLp_ == 0.0f && flowBp_.ic1 == 0.0f && flowBp_.ic2 == 0.0f
		       && flowBpR_.ic1 == 0.0f && flowBpR_.ic2 == 0.0f;
	case Kind::Friction:
		for (const Svf& r : fres_) {
			if (r.ic1 != 0.0f || r.ic2 != 0.0f) {
				return false;
			}
		}
		return body_.v == 0.0f && fricLp_ == 0.0f;
	case Kind::Particle:
		for (const Svf& r : pres_) {
			if (r.ic1 != 0.0f || r.ic2 != 0.0f) {
				return false;
			}
		}
		return body_.v == 0.0f && grain_.v == 0.0f && partLp_ == 0.0f;
	default: return true;
	}
}

void Hit::checkSilence() {
	// A MODE BELOW THE FLOOR IS EXACTLY ZERO (proposal §4.4), so the bank can be found silent and no denormal is left
	// rotating.
	for (int i = 0; i < modeCount_; ++i) {
		const auto u = static_cast<size_t>(i);
		if (zr_[u] * zr_[u] + zi_[u] * zi_[u] < kSilenceFloor * kSilenceFloor) {
			zr_[u] = 0.0f;
			zi_[u] = 0.0f;
		}
	}
	if (kind_ == Kind::Sweep && std::fabs(sweepFilter_.ic1) < kSilenceFloor && std::fabs(sweepFilter_.ic2) < kSilenceFloor) {
		sweepFilter_.clear();
	}
	if (kind_ == Kind::Water) {
		if (std::fabs(splashLp_) < kSilenceFloor) {
			splashLp_ = 0.0f;
		}
		for (Svf* r : { &flowBp_, &flowBpR_ }) {
			if (std::fabs(r->ic1) < kSilenceFloor && std::fabs(r->ic2) < kSilenceFloor) {
				r->clear();
			}
		}
		// A DECAYING SHOWER ENDS WHEN NO EVENT CAN STILL COME and its bed is below hearing: the drops still ringing finish on their own.
		if (body_.v > 0.0f && body_.d < 1.0f) {
			const double remaining = laws_.waterRate / sr_ * static_cast<double>(body_.v) / (1.0 - static_cast<double>(body_.d));
			const bool noMore = static_cast<double>(partAcc_) + remaining < static_cast<double>(partThresh_);
			if (noMore && (laws_.flow == 0.0 || body_.v < 1.0e-7f)) {
				body_.v = 0.0f;
			}
		}
	}
	if (kind_ == Kind::Friction) {
		for (Svf& r : fres_) {
			if (std::fabs(r.ic1) < kSilenceFloor && std::fabs(r.ic2) < kSilenceFloor) {
				r.clear();
			}
		}
		if (std::fabs(fricLp_) < kSilenceFloor) {
			fricLp_ = 0.0f;
		}
		// A BOW'S STRENGTH BELOW A TENTH OF A MICROUNIT IS NONE (its resonances are far below the floor by then): a spent bow ends when its sound does.
		if (body_.d < 1.0f && body_.v < 1.0e-7f) {
			body_.v = 0.0f;
		}
	}
	if (kind_ == Kind::Particle) {
		// A RESONANCE BELOW THE FLOOR IS EXACTLY ZERO, so a spent shower can be found silent and no denormal is left ringing.
		for (Svf& r : pres_) {
			if (std::fabs(r.ic1) < kSilenceFloor && std::fabs(r.ic2) < kSilenceFloor) {
				r.clear();
			}
		}
		if (std::fabs(partLp_) < kSilenceFloor) {
			partLp_ = 0.0f;
		}
		// A DECAYING SHOWER ENDS WHEN NO COLLISION CAN STILL COME: the counter still to gain, summed over the density envelope's whole fall,
		// cannot reach the threshold. (A held or droning shower's envelope does not fall, so this never ends it.)
		if (body_.v > 0.0f && body_.d < 1.0f) {
			const double remaining = laws_.density / sr_ * static_cast<double>(body_.v) / (1.0 - static_cast<double>(body_.d));
			if (static_cast<double>(partAcc_) + remaining < static_cast<double>(partThresh_)) {
				body_.v = 0.0f;
			}
		}
	}
	if (envelopesSilent()) {
		reset();
	}
}

float Hit::renderFm() {
	const float amp = body_.next();
	// THE INDEX, a sample at a time: the held share plus the rest falling by the index's own T60 (a Drone sits at its held share, its envelope at 0).
	const double held = laws_.play == kPlayOneShot ? 0.0 : kFmHeldShare;
	fmIndexNow_ = laws_.fmIndex * fmToneScale_ * (held + (1.0 - held) * static_cast<double>(fmIdxEnv_.next()));
	// THE PHASES ARE READ BEFORE THEY ADVANCE, so the strike's first sample is sin(0): exactly zero, and a hit never clicks by itself.
	// THE MODULATOR'S FEEDBACK: its previous output leans on its own phase, up to pi at full (the FM operator's own).
	const auto m = static_cast<float>(std::sin(kTwoPi * fmPhaseM_ + laws_.fmFeedback * std::numbers::pi * static_cast<double>(fmPrev_)));
	fmPrev_ = m;
	const auto carrier = static_cast<float>(std::sin(kTwoPi * fmPhaseC_ + fmIndexNow_ * static_cast<double>(m)));
	fmPhaseM_ += fmIncM_;
	fmPhaseM_ -= std::floor(fmPhaseM_);
	fmPhaseC_ += fmIncC_;
	fmPhaseC_ -= std::floor(fmPhaseC_);
	// THE NOISE SHARE takes from the carrier what its transient adds, so the hit's level stays where Level put it.
	float y = static_cast<float>(laws_.play == kPlayDrone ? kFmDroneGain : kFmGain) * amp * (1.0f - 0.5f * laws_.noise) * carrier * level_;
	// A SPENT TAP OR BAND DRAWS NO NOISE (its envelope is exactly zero): the draws are still the hit's own and in the same order on every pass.
	if (click_.v != 0.0f) {
		clickLp_ += clickCoef_ * (noise() - clickLp_);
		y += click_.next() * clickLp_;
	}
	if (band_.v != 0.0f) {
		y += 2.0f * band_.next() * bp_.bandPass(noise());
	}
	// A GUARD, NOT A SHAPER: the carrier, the tap and the noise are sized to fit, so this binds only at the extremes of every control at once.
	return std::clamp(y, -1.0f, 1.0f);
}

float Hit::renderSweep() {
	// THE POSITION: a sample at a time. A One-shot or a Gate runs to the end once (a One-shot then closes in 8 ms, a Gate holds); a Drone ping-pongs.
	if (laws_.play == kPlayDrone) {
		sweepPos_ += sweepDir_ * sweepStep_;
		if (sweepPos_ >= 1.0) {
			sweepPos_ = 1.0;
			sweepDir_ = -1.0;
		}
		else if (sweepPos_ <= 0.0) {
			sweepPos_ = 0.0;
			sweepDir_ = 1.0;
		}
	}
	else if (sweepPos_ < 1.0) {
		sweepPos_ = std::min(1.0, sweepPos_ + sweepStep_);
		if (sweepPos_ >= 1.0 && laws_.play == kPlayOneShot && !sweepEnded_) {
			sweepEnded_ = true;
			body_.d = envDecay(0.008, sr_); // the end of the sweep: closed in 8 ms (a riser's top is cut, not left ringing)
		}
	}
	const float amp = body_.next();
	// THE SOURCE: a saw (PolyBLEP-corrected) against white noise by the Noise field.
	sweepOscPhase_ += sweepOscInc_;
	sweepOscPhase_ -= std::floor(sweepOscPhase_);
	const double t = sweepOscPhase_;
	const double dt = sweepOscInc_;
	double blep = 0.0;
	if (t < dt) {
		const double x = t / dt;
		blep = x + x - x * x - 1.0;
	}
	else if (t > 1.0 - dt) {
		const double x = (t - 1.0) / dt;
		blep = x * x + x + x + 1.0;
	}
	const float saw = static_cast<float>(2.0 * t - 1.0 - blep);
	const float balance = laws_.noise;
	// THE STRENGTH GOES IN BEFORE THE FILTER, so a closed sweep's filter rings down to exact zero rather than being fed for ever.
	const float x = amp * ((1.0f - balance) * 0.6f * saw + balance * noise());
	float band = 0.0f;
	float low = 0.0f;
	sweepFilter_.step(x, band, low);
	sweepShape_ += sweepShapeStep_;
	return std::tanh((sweepLowMix_ * low + sweepBandMix_ * band) * sweepGain_ * sweepShape_) * level_; // a soft ceiling: a resonant sweep cannot pass full scale
}

void Hit::renderWater(float& left, float& right) {
	const float env = body_.next();
	// EVENTS: a Poisson process of Rate · the shower's strength. One is a chirp (a bubble's sine, rising from its size's pitch over its own life),
	// born with a place in the field and a size within Metal's spread, and a splash (a short noise transient) of Splash's size.
	partAcc_ += static_cast<float>(laws_.waterRate / sr_) * env;
	if (partAcc_ >= partThresh_) {
		partAcc_ -= partThresh_;
		const auto u01 = [&] { return std::clamp(0.5 * (static_cast<double>(partRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6); };
		partThresh_ = static_cast<float>(-std::log(1.0 - u01()));
		const double spread = static_cast<double>(laws_.metal);
		const double hz = std::min(laws_.hz * std::exp2(1.2 * spread * (2.0 * u01() - 1.0)), kTopFraction * sr_);
		// THE LIFE of a chirp: shorter for a small bubble, longer when soft (Snap down); it rises Rise · 2.2 octaves over it.
		const double life = std::clamp((0.012 + 0.08 * (1.0 - static_cast<double>(laws_.snap))) * std::sqrt(900.0 / hz), 0.004, 0.4);
		const double octaves = 2.2 * laws_.rise;
		WaterChirp* take = nullptr;
		for (WaterChirp& c : chirps_) {
			if (!c.on) {
				take = &c;
				break;
			}
			if (take == nullptr || c.amp < take->amp) {
				take = &c;
			}
		}
		const float pan = static_cast<float>((2.0 * u01() - 1.0) * spread);
		const double theta = (static_cast<double>(pan) + 1.0) * std::numbers::pi / 4.0;
		take->on = true;
		take->phase = 0.0;
		take->inc = hz / sr_;
		// the per-sample factor that carries it `octaves` up by the end of its life (T60 mapped: the chirp's amplitude is 1/1000 there)
		take->grow = std::exp2(octaves / (life * sr_));
		take->octavesLeft = octaves;
		take->amp = static_cast<float>(0.5 + 0.5 * u01());
		take->decay = envDecay(life, sr_);
		take->gl = static_cast<float>(std::cos(theta) * std::numbers::sqrt2);
		take->gr = static_cast<float>(std::sin(theta) * std::numbers::sqrt2);
		splashEnv_.v = std::min(1.5f, splashEnv_.v + static_cast<float>(laws_.splash * (0.5 + 0.5 * u01())));
		splashL_ = take->gl;
		splashR_ = take->gr;
		lastStartHz_ = hz;
		lastRiseOct_ = octaves;
		lastPan_ = pan;
		++events_;
	}
	float l = 0.0f;
	float r = 0.0f;
	for (WaterChirp& c : chirps_) {
		if (!c.on) {
			continue;
		}
		const float y = static_cast<float>(std::sin(kTwoPi * c.phase)) * c.amp;
		c.phase += c.inc;
		c.phase -= std::floor(c.phase);
		c.inc = std::min(c.inc * c.grow, 0.45);
		c.amp *= c.decay;
		if (c.amp < kSilenceFloor) {
			c = {};
		}
		l += y * c.gl;
		r += y * c.gr;
	}
	// THE SPLASH: noise under a fast envelope, coloured by Tone, at the place of the event that made it.
	const float sp = splashEnv_.next();
	splashLp_ += splashLpCoef_ * (noise() * sp - splashLp_);
	l += splashLp_ * splashL_ * 0.7f;
	r += splashLp_ * splashR_ * 0.7f;
	// THE FLOW: a band-limited bed, an independent noise for each ear, its level breathing slowly (a low-passed noise, about 3 Hz).
	if (flowGain_ > 0.0f) {
		flowWobble_ += flowWobbleCoef_ * (fricRand_.next() - flowWobble_);
		const float swell = 0.75f + 0.5f * flowWobble_;
		// THE STRENGTH GOES IN BEFORE THE FILTER, so a spent shower's bed rings down to exact zero rather than being fed for ever.
		l += flowGain_ * swell * flowBp_.bandPass(env * noise());
		r += flowGain_ * swell * flowBpR_.bandPass(env * waterNoiseR_.next());
	}
	// A SOFT CEILING on the sum of bed, splashes and drops (up to eight chirps pile up at a high Rate): never past full scale.
	left = std::tanh(l * 0.6f) * level_;
	right = std::tanh(r * 0.6f) * level_;
}

float Hit::renderFriction() {
	const float env = body_.next();
	// THE SLIPS: one each period. The period is the resonance's own at full Speed (the bow locks to it and a tone sustains) and lengthens toward
	// a creak's tick as Speed falls; the roughness jitters it. Whether a slip lets go at all grows with Speed and Pressure, so a slow, light bow
	// stutters. A slip is an impulse of the size the pressure and speed leave it.
	slipCount_ -= 1.0;
	float exc = 0.0f;
	if (slipCount_ <= 0.0) {
		const auto u01 = [&] { return std::clamp(0.5 * (static_cast<double>(fricRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6); };
		const double rough = static_cast<double>(laws_.noise);
		const double lockHz = std::max(20.0, laws_.hz);
		const double rate = std::exp(std::log(7.0) + (std::log(lockHz) - std::log(7.0)) * std::pow(laws_.speed, 1.5));
		slipPeriod_ = sr_ / rate * (1.0 + rough * 0.8 * (2.0 * u01() - 1.0));
		slipCount_ += std::max(1.0, slipPeriod_);
		const double chance = std::clamp(2.0 * laws_.speed, 0.15, 1.0) * std::clamp(0.4 + laws_.pressure, 0.0, 1.0);
		if (u01() <= chance) {
			slipAmp_ = static_cast<float>((0.25 + 0.75 * laws_.speed) * (0.2 + 0.8 * laws_.pressure) * (1.0 - rough * 0.6 * u01()));
			exc = slipAmp_ * env;
			++slips_;
		}
	}
	// THE SCRAPE: the roughness as a hiss under the slips, as bright as the slip.
	const float scrape = noise() * laws_.noise * env * 0.08f;
	fricLp_ += fricLpCoef_ * (exc + scrape - fricLp_);
	float y = 0.0f;
	for (int i = 0; i < kFrictionResonators; ++i) {
		const auto u = static_cast<size_t>(i);
		y += fresGain_[u] * fresQGain_[u] * fres_[u].bandPass(fricLp_);
	}
	return std::tanh(y * kFrictionGain + 0.25f * fricLp_) * level_; // a soft ceiling: unity when small, never past full scale
}

float Hit::renderParticle() {
	// COLLISIONS: the counter fills at density · envelope per second and fires at its exponentially drawn threshold, so the rate follows the envelope
	// exactly (a Poisson process of time-varying rate). A grain adds a burst of the size Snap leaves it (Snap 0: all alike; 1: uniformly unequal).
	const float env = body_.next();
	partAcc_ += static_cast<float>(laws_.density / sr_) * env;
	if (partAcc_ >= partThresh_) {
		partAcc_ -= partThresh_;
		const double u = std::clamp(0.5 * (static_cast<double>(partRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6);
		const double size = 1.0 - static_cast<double>(laws_.snap) * (1.0 - u);
		grain_.v = std::min(2.0f, grain_.v + static_cast<float>(size));
		partThresh_ = static_cast<float>(-std::log(1.0 - std::clamp(0.5 * (static_cast<double>(partRand_.next()) + 1.0), 1.0e-6, 1.0 - 1.0e-6)));
		++grains_;
	}
	// THE EXCITATION: white noise under the grains' envelope, its brightness set by Tone.
	const float x = noise() * grain_.next();
	partLp_ += partLpCoef_ * (x - partLp_);
	float y = 0.0f;
	for (int i = 0; i < kParticleResonators; ++i) {
		const auto u = static_cast<size_t>(i);
		y += presGain_[u] * presQGain_[u] * pres_[u].bandPass(partLp_);
	}
	// THE NOISE FIELD is the raw excitation's share, the rattle itself, against the resonances' pitched ring.
	const float w = laws_.noise;
	const float mixed = (1.0f - 0.7f * w) * y * kParticleGain + w * 2.0f * partLp_;
	// A SOFT CEILING: unity at small signals, and never past full scale however many grains pile up.
	return std::tanh(mixed) * level_;
}

float Hit::renderKick() {
	// THE BODY BY ROTATION, its self-modulation by short series (measured 2026-11-05: two double sines and a tanh a sample cost a
	// Kick about 10 µs a 256-frame block): (c, s) is (cos, sin) of the phase, turned by the block's increment each sample and
	// renormalised each control block; sin(φ + a·sin φ) = s·cos(a·s) + c·sin(a·s), with |a·s| ≤ 1.2, where the series below
	// are good to 2e-6.
	const float s = ks_;
	const float c = kc_;
	const float a = 1.2f * laws_.tone * s;
	const float a2 = a * a;
	const float sinA = a * (1.0f - a2 / 6.0f * (1.0f - a2 / 20.0f * (1.0f - a2 / 42.0f)));
	const float cosA = 1.0f - a2 / 2.0f * (1.0f - a2 / 12.0f * (1.0f - a2 / 30.0f));
	float y = (s * cosA + c * sinA) * body_.next() * dipGain_;
	dipGain_ += dipStep_;
	kc_ = c * rotC_ - s * rotS_;
	ks_ = c * rotS_ + s * rotC_;
	if (click_.v != 0.0f) {
		clickLp_ += clickCoef_ * (noise() - clickLp_);
		y += click_.next() * clickLp_;
	}
	if (band_.v != 0.0f) {
		y += band_.next() * noise();
	}
	return y;
}

float Hit::wires(float head) {
	// THE SNARE'S WIRES (doc §13 items 7 and 9): their envelope by Contour (Exponential, Linear, Modulated), scaled by the
	// buzz, through the band; with Width, the right ear's own wires beside them.
	float env = 0.0f;
	if (contour_ == 1) {
		env = wireLin_;
		wireLin_ = std::max(0.0f, wireLin_ - wireLinStep_);
		band_.v = wireLin_; // the silence test reads band_: it lands at exact zero with the line
	}
	else {
		if (band_.v == 0.0f) {
			wireEnvUsed_ = 0.0f;
			side_ = 0.0f;
			return 0.0f; // a spent band draws no noise (renderModes' rule)
		}
		env = band_.next();
		if (contour_ == 2) {
			// MODULATED: chopped by a falling sawtooth at kWireModHz, so the wires rattle in teeth rather than hiss.
			env *= 1.0f - kWireModDepth * modPhase_;
			modPhase_ += modInc_;
			if (modPhase_ >= 1.0f) {
				modPhase_ -= 1.0f;
			}
		}
	}
	if (buzzAmount_ > 0.0f) {
		// THE BUZZ (our own derivation): the wires follow the head's rectified swing, engaging only above Threshold (the
		// snare tension), so a soft hit's wires barely sound and a hard one's slap.
		buzzFollow_ += buzzCoef_ * (std::abs(head) - buzzFollow_);
		const float g = std::clamp((buzzFollow_ - buzzKnee_) / buzzSpan_, 0.0f, 1.0f);
		buzzUsed_ = (1.0f - buzzAmount_) + buzzAmount_ * g;
		env *= buzzUsed_;
	}
	wireEnvUsed_ = env;
	const float l = bp_.bandPass(noise());
	const float wire = 2.0f * env * l;
	if (widthOn_) {
		const float r = widthC_ * l + widthS_ * bpR_.bandPass(noiseR_.next());
		side_ = 2.0f * env * r - wire;
	}
	return wire;
}

float Hit::renderModes() {
	float y = 0.0f;
	for (int i = 0; i < modeCount_; ++i) {
		const auto u = static_cast<size_t>(i);
		const float r = zr_[u] * cr_[u] - zi_[u] * ci_[u];
		const float m = zr_[u] * ci_[u] + zi_[u] * cr_[u];
		zr_[u] = r;
		zi_[u] = m;
		y += m;
	}
	const float head = y; // the head alone, before its click and wires: what the wires buzz against
	lastHead_ = head;
	// A SPENT CLICK OR BAND DRAWS NO NOISE (its envelope is exactly zero, so it would add exactly zero): the draws are still
	// the hit's own and in the same order on every pass, so the hit is as deterministic as before.
	if (click_.v != 0.0f) {
		clickLp_ += clickCoef_ * (noise() - clickLp_);
		y += click_.next() * clickLp_;
	}
	if (kind_ == Kind::Snare) {
		y += wires(head);
	}
	else if (band_.v != 0.0f) {
		y += 2.0f * band_.next() * bp_.bandPass(noise());
	}
	// THE MOVING BELL (a Snare's, drum proposal §12): the whole hit through a resonant band-pass, normalised to unity at its
	// centre (k · v1), added at up to four times: +14 dB at the peak at Bell 100%.
	if (bellOn_ || bell2On_) {
		const float dry = y; // both bells hear the hit itself, not each other
		if (bellOn_) {
			y += bellGain_ * bell_.k * bell_.bandPass(dry);
		}
		if (bell2On_) {
			// THE SECOND BELL (Requirement: two moving peaks): its own Q, and its own ring, a level falling 60 dB in Bell 2 Time.
			bell2Used_ = bell2Gain_ * bell2Env_.next();
			y += bell2Used_ * bell2_.k * bell2_.bandPass(dry);
		}
	}
	return y;
}

float Hit::renderMetal() {
	// IN FLOAT (measured 2026-11-05: the double phases and blep cost a Metal hit about 7 µs a block): a phase in [0, 1) wraps
	// every period, so float's 24 bits are 1e-7 of a cycle, far inside the blep's width.
	// FOUR LANES AT A TIME, BRANCH-FREE (the partial bank's road): the blep's two sides are masks, its divisions a reciprocal per
	// block. Two vectors: the six oscillators and two silent lanes of weight 0.
	const __m128 one = _mm_set1_ps(1.0f);
	const __m128 two = _mm_set1_ps(2.0f);
	const __m128 half = _mm_set1_ps(0.5f);
	__m128 acc = _mm_setzero_ps();
	for (size_t u = 0; u < kLanes; u += 4) {
		const __m128 p = _mm_load_ps(mphaseF_.data() + u);
		const __m128 dt = _mm_load_ps(mincF_.data() + u);
		const __m128 inv = _mm_load_ps(invDtF_.data() + u);
		const __m128 w = _mm_load_ps(laneW_.data() + u);
		__m128 q = _mm_add_ps(p, half);
		q = _mm_sub_ps(q, _mm_and_ps(_mm_cmpge_ps(q, one), one));
		const __m128 hiEdge = _mm_sub_ps(one, dt);
		const auto blep = [&](__m128 t) {
			const __m128 x = _mm_mul_ps(t, inv);
			const __m128 y = _mm_mul_ps(_mm_sub_ps(t, one), inv);
			const __m128 lo = _mm_sub_ps(_mm_sub_ps(_mm_add_ps(x, x), _mm_mul_ps(x, x)), one);
			const __m128 hi = _mm_add_ps(_mm_add_ps(_mm_mul_ps(y, y), _mm_add_ps(y, y)), one);
			return _mm_add_ps(_mm_and_ps(_mm_cmplt_ps(t, dt), lo), _mm_and_ps(_mm_cmpgt_ps(t, hiEdge), hi));
		};
		const __m128 square = _mm_sub_ps(_mm_and_ps(_mm_cmplt_ps(p, half), two), one);
		acc = _mm_add_ps(acc, _mm_mul_ps(w, _mm_add_ps(square, _mm_sub_ps(blep(p), blep(q)))));
		__m128 next = _mm_add_ps(p, dt);
		next = _mm_sub_ps(next, _mm_and_ps(_mm_cmpge_ps(next, one), one));
		_mm_store_ps(mphaseF_.data() + u, next);
	}
	alignas(16) float lane[4];
	_mm_store_ps(lane, acc);
	const float sum = (lane[0] + lane[1]) + (lane[2] + lane[3]);
	const float mix = laws_.noise > 0.0f ? (1.0f - laws_.noise) * sum + laws_.noise * noise() : sum;
	float x = bp_.bandPass(mix);
	hpState_ += hpCoef_ * (x - hpState_);
	x -= hpState_;
	return 2.0f * x * (body_.next() + click_.next());
}

float Hit::clapTooth() {
	// THE 808'S TOOTH (checked against its schematic's envelope, drum proposal §12): each hand is a SAWTOOTH, an instant rise
	// and a fall that lands at zero just as the next hand starts, not an exponential that overlaps it. At Snap 0 the fall is
	// the 808's straight line; Snap bends it toward r², a sharper hand.
	if (!toothOn_) {
		toothValue_ = 0.0f;
		return 0.0f;
	}
	const float r = 1.0f - static_cast<float>(toothK_) * toothInv_;
	++toothK_;
	if (toothK_ >= toothSpan_) {
		toothOn_ = false;
	}
	toothValue_ = level_ * r * ((1.0f - laws_.snap) + laws_.snap * r);
	return toothValue_;
}

float Hit::renderClap() {
	if (nextBurst_ < laws_.count && onsets_[static_cast<size_t>(nextBurst_)] == age_) {
		// The tooth spans the gap to the next hand; the last spans the gap before it (a lone hand, 10 ms).
		const auto i = static_cast<size_t>(nextBurst_);
		int span = 0;
		if (nextBurst_ + 1 < laws_.count) {
			span = onsets_[i + 1] - onsets_[i];
		}
		else if (nextBurst_ > 0) {
			span = onsets_[i] - onsets_[i - 1];
		}
		else {
			span = static_cast<int>(std::lround(0.010 * sr_));
		}
		span = std::max(span, static_cast<int>(std::lround(0.001 * sr_)));
		toothK_ = 0;
		toothSpan_ = span;
		toothInv_ = 1.0f / static_cast<float>(span);
		toothOn_ = true;
		++nextBurst_;
		if (nextBurst_ == laws_.count) {
			tail_.set(level_ * (0.3f + 0.7f * laws_.noise), effective(laws_.t60), sr_);
		}
	}
	const float env = std::max(clapTooth(), tail_.next());
	const float lBand = bp_.bandPass(noise());
	float x = lBand;
	if (clapHpCoef_ > 0.0f) {
		clapHpLp_ += clapHpCoef_ * (x - clapHpLp_);
		x -= clapHpLp_;
	}
	if (widthOn_) {
		// WIDTH: the right ear's band, cos·(the left's) + sin·(its own), through its own high-pass.
		float xr = widthC_ * lBand + widthS_ * bpR_.bandPass(noiseR_.next());
		if (clapHpCoef_ > 0.0f) {
			clapHpLpR_ += clapHpCoef_ * (xr - clapHpLpR_);
			xr -= clapHpLpR_;
		}
		side_ = clapGain_ * env * (xr - x);
	}
	return clapGain_ * env * x;
}

void Hit::driveSpan(float* out, int n, float& lp) {
	// THE TONE BEFORE THE DRIVE: x + tilt·(x − 2·lp), so −1 is twice the lows, 1 twice the highs; flat (0) is skipped, so the
	// default drive is the old Kick's to the bit.
	if (driveTilt_ != 0.0f) {
		for (int i = 0; i < n; ++i) {
			lp += driveLpCoef_ * (out[i] - lp);
			out[i] += driveTilt_ * (out[i] - 2.0f * lp);
		}
	}
	const float g = driveGain_;
	const float norm = driveNorm_;
	switch (driveCurve_) {
	case 1:
		for (int i = 0; i < n; ++i) {
			out[i] = (dsp::fasttanh(std::clamp(g * out[i] + driveBias_, -4.5f, 4.5f)) - driveOffset_) * norm;
		}
		break;
	case 2:
		for (int i = 0; i < n; ++i) {
			const float u = std::clamp(g * out[i], -1.0f, 1.0f);
			out[i] = (1.5f * u - 0.5f * u * u * u) * norm;
		}
		break;
	default:
		for (int i = 0; i < n; ++i) {
			out[i] = dsp::fasttanh(std::clamp(g * out[i], -4.5f, 4.5f)) * norm; // its valid range; tanh(4.5) is 0.99975
		}
		break;
	}
}

void Hit::render(float* out, int n, float* outR) {
	int s = 0;
	while (s < n) {
		if (idle_) {
			std::fill(out + s, out + n, 0.0f);
			if (outR != nullptr) {
				std::fill(outR + s, outR + n, 0.0f);
			}
			return;
		}
		if (controlLeft_ <= 0) {
			if (kind_ == Kind::Kick) {
				// THE ROTATOR'S LENGTH HELD AT 1 against rounding, every block whether or not the laws moved.
				const float m = std::sqrt(kc_ * kc_ + ks_ * ks_);
				if (m > 0.0f) {
					kc_ /= m;
					ks_ /= m;
				}
			}
			control();
			checkSilence();
			controlLeft_ = kControlBlock;
			continue;
		}
		const int span = std::min(controlLeft_, n - s);
		switch (kind_) {
		case Kind::Kick:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderKick();
			}
			break;
		case Kind::Membrane:
		case Kind::Snare:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderModes();
				if (outR != nullptr) {
					outR[s + i] = out[s + i] + (widthOn_ ? side_ : 0.0f);
				}
			}
			break;
		case Kind::Metal:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderMetal();
			}
			break;
		case Kind::Clap:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderClap();
				if (outR != nullptr) {
					outR[s + i] = out[s + i] + (widthOn_ ? side_ : 0.0f);
				}
				++age_;
			}
			age_ -= span;
			break;
		case Kind::Particle:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderParticle();
			}
			break;
		case Kind::Friction:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderFriction();
			}
			break;
		case Kind::Sweep:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderSweep();
			}
			break;
		case Kind::Hand:
			for (int i = 0; i < span; ++i) {
				out[s + i] = handCeiling(renderModes());
			}
			break;
		case Kind::FM:
			for (int i = 0; i < span; ++i) {
				out[s + i] = renderFm();
			}
			break;
		case Kind::Water:
			for (int i = 0; i < span; ++i) {
				float right = 0.0f;
				renderWater(out[s + i], right);
				if (outR != nullptr) {
					outR[s + i] = right;
				}
			}
			break;
		case Kind::String:
			if (string_ != nullptr) {
				string_->render(out + s, nullptr, span);
			}
			else {
				std::fill(out + s, out + s + span, 0.0f);
			}
			break;
		default: std::fill(out + s, out + s + span, 0.0f); break;
		}
		// THE RIGHT EAR of every kind with no Width of its own is the left.
		if (outR != nullptr && kind_ != Kind::Membrane && kind_ != Kind::Snare && kind_ != Kind::Clap && kind_ != Kind::Water) {
			std::copy(out + s, out + s + span, outR + s);
		}
		if (driveGain_ > 0.0f) {
			driveSpan(out + s, span, driveLp_);
			if (outR != nullptr) {
				driveSpan(outR + s, span, driveLpR_);
			}
		}
		age_ += span;
		controlLeft_ -= span;
		s += span;
	}
}

double Hit::currentHz(int mode) const {
	switch (kind_) {
	case Kind::Kick: return inc_ * sr_;
	case Kind::Membrane:
	case Kind::Hand:
	case Kind::Snare: return modeHz_[static_cast<size_t>(mode)];
	case Kind::FM: return fmIncC_ * sr_;
	case Kind::String: return string_ != nullptr ? string_->frequencyHz() : 0.0;
	default: return laws_.hz;
	}
}

float Hit::modeAmplitude(int k) const {
	const auto u = static_cast<size_t>(k);
	return std::sqrt(zr_[u] * zr_[u] + zi_[u] * zi_[u]);
}

double Hit::modeDecayPerSample(int k) const { return modeR_[static_cast<size_t>(k)]; }

double Hit::metalHz(int i) const { return minc_[static_cast<size_t>(i)] * sr_; }

} // namespace feathers::drum
