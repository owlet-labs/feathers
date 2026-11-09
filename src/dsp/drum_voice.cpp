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
/// ±2% of the tension glide's depth, seeded (doc §13 item 10).
constexpr double kTensionVariation = 0.02;
/// A tension glide's resolution: half a cent (1/2400 of an octave).
constexpr double kTensionSteps = 2400.0;
/// Width's second noise (the right ear's wires or clap band): its own stream, so the left is the mono hit to the bit.
constexpr uint64_t kWidthSalt = 0x5E7E0D1DE0ull;
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
	default: return { false, false, false, false, false, false, false };
	}
}

ControlUse kindControls(Kind k) {
	ControlUse u {};
	// DRIVE ON EVERY KIND, with its curve and the tone before it.
	u.drive = k != Kind::kCount;
	// SWEEP TIME: the Kick's pitch envelope and the String's; a Membrane's or a Snare's glide follows its own ring (Tension).
	u.sweepTime = k == Kind::Kick || k == Kind::String;
	u.count = u.spread = u.clapFilter = k == Kind::Clap;
	u.snareEnvelopes = u.bell = k == Kind::Snare;
	u.kickShape = k == Kind::Kick;
	u.tension = k == Kind::Kick || k == Kind::Membrane || k == Kind::Snare;
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
	default: return 110.0;
	}
}

double kindSweepOctaves(Kind k) {
	switch (k) {
	case Kind::Kick: return 4.0;
	case Kind::Membrane: return 1.0;
	case Kind::Snare: return 0.5;
	case Kind::String: return 1.0;
	default: return 0.0;
	}
}

namespace {

struct ZoneSpot {
	size_t at;
	size_t next;
	double frac;
};

/// Where a coordinate stands among `n` zones: its lap's fractional part, the zone it is in, the next, and how far across.
ZoneSpot zoneSpot(double coordinate, size_t n) {
	const double u = coordinate - std::floor(coordinate);
	const double z = u * static_cast<double>(n);
	const size_t at = std::min(static_cast<size_t>(std::max(0.0, std::floor(z))), n - 1);
	return { at, (at + 1) % n, z - static_cast<double>(at) };
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

Laws lawsFor(Kind k, const Fields& f, const Controls& c, double baseHz, double velocity) {
	Laws l;
	l.hz = baseHz * std::pow(2.0, (static_cast<double>(f.pitch) - 0.5) * 2.0 * c.pitchRangeSemitones / 12.0);
	l.t60 = c.decaySeconds * std::pow(4.0, 2.0 * static_cast<double>(f.length) - 1.0);
	// THE THREE ZONES, read once (drum proposal §14): what velocity reaches, the wires' character, the bells.
	const VelocityMix vel = velocityAt(c.velocityZone);
	const WiresMix wire = wiresAt(c.wires);
	const BellsMix bell = bellsAt(c.bells);
	const double vt = vel.tone * (velocity - 0.5);
	l.tone = clamp01(static_cast<double>(f.tone) + vt);
	l.snap = clamp01(static_cast<double>(f.snap) + vt);
	l.sweepOctaves = static_cast<double>(f.sweep) * kindSweepOctaves(k);
	l.sweepTau = std::max(1.0e-4, c.sweepSeconds / 3.0);
	if (k == Kind::Kick || k == Kind::Membrane || k == Kind::Snare) {
		// TENSION (doc §13 item 1, Avanzini): a struck head's pitch offset goes as its amplitude SQUARED, so the glide's depth
		// is velocity² (a hard hit drops further by itself), Velocity Tension setting how much of that law applies. On a
		// Membrane or a Snare the glide follows the head's own envelope squared, 10^(−6t/T60), so it is as long as the drum
		// rings: no Sweep Time. The Kick keeps its Sweep Time (its pitch envelope is the kick's character).
		const double v = std::clamp(velocity, 0.0, 1.0);
		l.sweepOctaves *= std::max(0.0, 1.0 + std::clamp(vel.tension, 0.0, 1.0) * (v * v - 1.0));
		if (k != Kind::Kick) {
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
	return l;
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
	decayT60_ = -1.0;
	body_ = {};
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
	const double scale = laws_.hz * std::exp2(bend);
	// THE RATIOS AND THE DECAYS ARE CACHED on what they read (Metal; the T60 and the Gate): a sweeping hit moves only its
	// frequencies, so only the rotations are computed again while it bends (measured: a burst of struck Membranes at 1024
	// frames cost about twice a resting hit's while every block re-ran sixteen pows for the ratios and sixteen for the decays).
	const double t60 = bodyGated_ ? std::min(effective(laws_.t60), kGateFall) : effective(laws_.t60);
	const bool ratiosStale = !(ratioMetal_ == laws_.metal) || ratioCount_ != count;
	if (ratiosStale) {
		for (int i = 0; i < count; ++i) {
			ratio_[static_cast<size_t>(i)] = modeRatio(i, laws_.metal);
		}
		ratioMetal_ = laws_.metal;
		ratioCount_ = count;
	}
	const bool decaysStale = ratiosStale || !(decayT60_ == t60);
	if (decaysStale) {
		for (int i = 0; i < count; ++i) {
			decayR_[static_cast<size_t>(i)] = decayFactor(modeT60(t60, ratio_[static_cast<size_t>(i)], laws_.metal), sr_);
		}
		decayT60_ = t60;
	}
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		const double ratio = ratio_[u];
		const double hz = scale * ratio;
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
	if (kind_ == Kind::Membrane || kind_ == Kind::Snare) {
		bend = std::round(bend * kTensionSteps) / kTensionSteps;
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
	const ControlKey key { laws_, bend, released_, releaseT60_, wireBend, bellHz, gates[0], gates[1], holding, dip };
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
	case Kind::Membrane:
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
	if (envelopesSilent()) {
		reset();
	}
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
		if (outR != nullptr && kind_ != Kind::Membrane && kind_ != Kind::Snare && kind_ != Kind::Clap) {
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
	case Kind::Snare: return modeHz_[static_cast<size_t>(mode)];
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
