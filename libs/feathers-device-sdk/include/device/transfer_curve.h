/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once


#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <utility>

// A ROUTE'S TRANSFER — the curve a route reads its source through before its amount: `amount × f(source) × span`.
//
// Lin is the plain road and is never called through `shape`: every resolve branches on Lin per INPUT and keeps
// its own expression, so a patch of Lin routes costs nothing per sample. Every other curve clamps its input to
// [−1, 1] first and maps 0 to 0, so a rested source still adds nothing to anything.
//
// **WHAT IS HERE AND WHAT IS THE HOST'S.** The curve CODES, their coefficients, labels and every curve's shape
// are here. The φ curve's EVALUATION is not: it is the host's character code, so `shapeTransferWith` takes it as
// a callable. A device never evaluates a route — the host does — so a device needs only the codes.

namespace feathers::device {

/// **A `Transfer` IS A CURVE CODE: THE KIND IN THE LOW BYTE, ITS COEFFICIENT'S STEP IN THE HIGH.** The
/// enumerators are the KINDS — the menu's order — each at its DEFAULT coefficient, so `Transfer::Log` is exactly
/// the default Log, and a code with a step is a different curve of the same kind. The coefficient is part of the
/// curve, so it rides every road that carries a route's curve unchanged. A file never sees a code: it writes the
/// kind's label and the coefficient as its own keyed value.
///
/// THE COEFFICIENTS, ALL OF THEM: Log and Exp have an EXPONENT (1 to 8, default 2: `1 − (1 − |x|)^k`, `|x|^k`);
/// Phi has its PERIODS (1 to 128 per swing, default 16). Lin, Rectify, Pos half and Neg half have none.
enum class Transfer : uint16_t { Lin, Log, Exp, Abs, Pos, Neg, Phi, kCount };

/// THE KINDS — the menu's rows, one per kind.
inline constexpr size_t kTransferCount = static_cast<size_t>(Transfer::kCount);
inline constexpr size_t kTransferMenuRows = kTransferCount;
inline constexpr size_t kTransferMenuPhiRow = static_cast<size_t>(Transfer::Phi);

/// WHAT THE FILE SAYS — by label, as Choices are (JF): a reorder of the enum is a non-event for a patch.
inline constexpr const char* const kTransferLabels[] = { "lin", "log", "exp", "abs", "pos", "neg", "phi" };
/// WHAT THE MENU SAYS.
inline constexpr const char* const kTransferNames[] = { "Linear", "Log", "Exp", "Rectify", "Pos half", "Neg half",
	                                                    "Phi random" };
static_assert(std::size(kTransferLabels) == kTransferCount);
static_assert(std::size(kTransferNames) == kTransferCount);
inline constexpr const char* kPhiTransferLabel = kTransferLabels[kTransferMenuPhiRow];
inline constexpr const char* kPhiTransferName = kTransferNames[kTransferMenuPhiRow];

/// THE CODE'S TWO HALVES.
[[nodiscard]] constexpr Transfer transferKind(Transfer t) noexcept {
	return static_cast<Transfer>(static_cast<uint16_t>(t) & 0xffu);
}
/// THE STEP STORED IN THE HIGH BYTE — signed for an exponent (0 is the default 2), unsigned for Phi's periods.
[[nodiscard]] constexpr int transferStep(Transfer t) noexcept {
	const auto high = static_cast<uint8_t>(static_cast<uint16_t>(t) >> 8);
	return transferKind(t) == Transfer::Phi ? static_cast<int>(high) : static_cast<int>(static_cast<int8_t>(high));
}

[[nodiscard]] constexpr bool hasCoefficient(Transfer t) noexcept {
	const Transfer k = transferKind(t);
	return k == Transfer::Log || k == Transfer::Exp || k == Transfer::Phi;
}
[[nodiscard]] constexpr bool isPhiTransfer(Transfer t) noexcept { return transferKind(t) == Transfer::Phi; }

// ---------------------------------------------------------------------------------------------------
// THE EXPONENT (Log and Exp): k = 2 · 2^(step / 16), steps −16..32 — 1 to 8, even in octaves, 2 at step 0.

inline constexpr float kTransferExponent = 2.0f; ///< THE DEFAULT, step 0 — MATH's `kMapExponent` too.
inline constexpr int kExpStepsPerOctave = 16;
inline constexpr int kExpStepMin = -16; ///< exponent 1: the curve is a line
inline constexpr int kExpStepMax = 32;  ///< exponent 8
inline constexpr float kExponentMin = 1.0f;
inline constexpr float kExponentMax = 8.0f;
// ---------------------------------------------------------------------------------------------------
// THE φ CURVE'S COEFFICIENT. Its periods are `exp2(v)`, v in 0..7 — 1 to 128 periods per full swing, even in
// octaves (`Unit::LogMultiple`). A route keeps v quantised to `kPhiStepsPerOctave`. The curve itself — one
// triangle read at a phase warped by two more at φ-ratio frequencies — is evaluated by the host.

/// 0..7 octaves: 1 to 128 periods.
inline constexpr float kPhiOctavesMax = 7.0f;
/// THE DEFAULT — 16 periods: past the point where the curve stops reading as a shape and starts reading as a
/// lookup, short of where a slow source sweeps it into buzz. PROVISIONAL, by ear later.
inline constexpr float kPhiOctavesDefault = 4.0f;
inline constexpr int kPhiStepsPerOctave = 32;
inline constexpr int kPhiSteps = static_cast<int>(kPhiOctavesMax) * kPhiStepsPerOctave; // the last step's index


// ---------------------------------------------------------------------------------------------------
// THE STEPS, BY KIND.

/// THE STEPS A KIND'S COEFFICIENT TAKES — {0, 0} for a kind with none.
[[nodiscard]] constexpr std::pair<int, int> transferStepRange(Transfer t) noexcept {
	switch (transferKind(t)) {
	case Transfer::Log:
	case Transfer::Exp: return { kExpStepMin, kExpStepMax };
	case Transfer::Phi: return { 0, kPhiSteps };
	default: return { 0, 0 };
	}
}

/// **THE CURVE OF `t`'S KIND AT `step`**, clamped into the kind's range; a kind with no coefficient is itself.
[[nodiscard]] constexpr Transfer transferWithStep(Transfer t, int step) noexcept {
	const auto [lo, hi] = transferStepRange(t);
	const int s = std::clamp(step, lo, hi);
	const auto high = static_cast<uint16_t>(static_cast<uint8_t>(static_cast<int8_t>(s)));
	const auto phiHigh = static_cast<uint16_t>(static_cast<uint8_t>(s));
	const uint16_t byte = transferKind(t) == Transfer::Phi ? phiHigh : high;
	return static_cast<Transfer>(static_cast<uint16_t>(transferKind(t)) | static_cast<uint16_t>(byte << 8));
}

/// **WHETHER A CODE IS A CURVE AT ALL** — a kind, and a step inside that kind's range. Everything else is a
/// number no menu, drag or file can produce, and every setter refuses it.
[[nodiscard]] constexpr bool transferValid(Transfer t) noexcept {
	if (static_cast<size_t>(transferKind(t)) >= kTransferCount) {
		return false;
	}
	const auto [lo, hi] = transferStepRange(t);
	const int s = transferStep(t);
	return s >= lo && s <= hi;
}

/// THE KIND AT ITS DEFAULT COEFFICIENT — what the menu picks for a kind you are not already on.
[[nodiscard]] constexpr Transfer transferDefaultOf(Transfer kind) noexcept {
	return transferKind(kind) == Transfer::Phi
	           ? transferWithStep(Transfer::Phi, static_cast<int>(kPhiOctavesDefault) * kPhiStepsPerOctave)
	           : transferKind(kind);
}

/// THE φ CURVE AT STEP `step` (0..kPhiSteps), clamped.
[[nodiscard]] constexpr Transfer phiTransferAtStep(int step) noexcept { return transferWithStep(Transfer::Phi, step); }
/// The step of a φ code (0 for anything else).
[[nodiscard]] constexpr int phiStep(Transfer t) noexcept { return isPhiTransfer(t) ? transferStep(t) : 0; }
/// THE φ CURVE NEAREST `octaves` (log2 of the periods) — a NaN is the default.
[[nodiscard]] inline Transfer phiTransfer(float octaves) noexcept {
	const float v = std::isnan(octaves) ? kPhiOctavesDefault : std::clamp(octaves, 0.0f, kPhiOctavesMax);
	return phiTransferAtStep(static_cast<int>(std::lround(v * static_cast<float>(kPhiStepsPerOctave))));
}
/// A φ code's octaves, log2 of its periods.
[[nodiscard]] inline float phiOctaves(Transfer t) noexcept {
	return static_cast<float>(phiStep(t)) / static_cast<float>(kPhiStepsPerOctave);
}
/// A φ code's periods per full swing.
[[nodiscard]] inline float phiPeriods(Transfer t) noexcept { return std::exp2(phiOctaves(t)); }

/// THE EXPONENT OF A Log OR Exp CODE (the default 2 for anything else).
[[nodiscard]] inline float transferExponent(Transfer t) noexcept {
	const Transfer k = transferKind(t);
	if (k != Transfer::Log && k != Transfer::Exp) {
		return kTransferExponent;
	}
	return kTransferExponent * std::exp2(static_cast<float>(transferStep(t)) / static_cast<float>(kExpStepsPerOctave));
}
/// THE Log OR Exp CURVE NEAREST `exponent` (clamped to 1..8; a NaN is the default 2). `kind` is Log or Exp.
[[nodiscard]] inline Transfer exponentTransfer(Transfer kind, float exponent) noexcept {
	const float e = std::isnan(exponent) ? kTransferExponent : std::clamp(exponent, kExponentMin, kExponentMax);
	return transferWithStep(kind, static_cast<int>(std::lround(std::log2(e / kTransferExponent)
	                                                            * static_cast<float>(kExpStepsPerOctave))));
}

/// **THE COEFFICIENT AS A FILE WRITES IT AND A HAND READS IT** — the exponent for Log/Exp, the octaves (log2 of
/// the periods) for Phi, and 0 for a kind with none. `transferWithCoefficient` is its inverse.
[[nodiscard]] inline float transferCoefficient(Transfer t) noexcept {
	switch (transferKind(t)) {
	case Transfer::Log:
	case Transfer::Exp: return transferExponent(t);
	case Transfer::Phi: return phiOctaves(t);
	default: return 0.0f;
	}
}
[[nodiscard]] inline Transfer transferWithCoefficient(Transfer kind, float value) noexcept {
	switch (transferKind(kind)) {
	case Transfer::Log:
	case Transfer::Exp: return exponentTransfer(transferKind(kind), value);
	case Transfer::Phi: return phiTransfer(value);
	default: return transferKind(kind);
	}
}
/// THE KEY A FILE STORES THE COEFFICIENT UNDER — a name per MEANING (an exponent is not an octave count), or
/// null for a kind with none.
[[nodiscard]] constexpr const char* transferCoefficientKey(Transfer t) noexcept {
	switch (transferKind(t)) {
	case Transfer::Log:
	case Transfer::Exp: return "transfer_exponent";
	case Transfer::Phi: return "transfer_octaves";
	default: return nullptr;
	}
}

/// **WHETHER THE CURVE CHANGES ANYTHING ON A SOURCE THAT NEVER GOES BELOW ZERO** — the rectifiers are an
/// identity (Rectify, Pos half) or silence (Neg half) there, so the menu greys them for a unipolar source.
[[nodiscard]] constexpr bool transferNeedsBipolar(Transfer t) noexcept {
	const Transfer k = transferKind(t);
	return k == Transfer::Abs || k == Transfer::Pos || k == Transfer::Neg;
}

/// **ONE SOURCE READING THROUGH `t`, WITH THE φ CURVE SUPPLIED** — `phiCurve(u, periods)` answers the φ curve
/// at magnitude u in 0..1 (the host's `shapeTransfer` passes its own). Lin answers `x` itself, unclamped — but no
/// resolve calls this for Lin; it is here for the picture and the cases. A code that is not a curve answers 0: a
/// stored number no menu can produce is silence, not a guess.
template <typename PhiCurve>
[[nodiscard]] inline float shapeTransferWith(Transfer t, float x, PhiCurve&& phiCurve) noexcept {
	if (t == Transfer::Lin) {
		return x;
	}
	if (!transferValid(t)) {
		return 0.0f;
	}
	const float c = std::clamp(x, -1.0f, 1.0f);
	const float m = c < 0.0f ? -c : c;
	// THE DEFAULT EXPONENT KEEPS ITS SQUARE — bit-identical to every route before the coefficient existed.
	const bool square = transferStep(t) == 0;
	switch (transferKind(t)) {
	case Transfer::Log: {
		const float u = 1.0f - m;
		const float bent = 1.0f - (square ? u * u : std::pow(u, transferExponent(t)));
		return c < 0.0f ? -bent : bent;
	}
	case Transfer::Exp: {
		const float bent = square ? m * m : std::pow(m, transferExponent(t));
		return c < 0.0f ? -bent : bent;
	}
	case Transfer::Abs: return m;
	case Transfer::Pos: return c > 0.0f ? c : 0.0f;
	case Transfer::Neg: return c < 0.0f ? c : 0.0f;
	case Transfer::Phi: {
		const float bent = phiCurve(m, phiPeriods(t));
		return c < 0.0f ? -bent : bent;
	}
	default: return 0.0f;
	}
}

/// FOR A READER THAT HANDLES φ BEFORE IT ASKS (a reach, whose φ extremes are not at the ends): the φ case
/// answers 0 and is never meant to be reached.
struct NoPhiCurve {
	float operator()(float, float) const noexcept { return 0.0f; }
};

/// THE LABEL A FILE WRITES — the KIND's, as a Choice label names its entry; the coefficient is its own key
/// (`transferCoefficientKey`). Null for a code that is not a curve.
[[nodiscard]] inline const char* transferLabel(Transfer t) noexcept {
	return transferValid(t) ? kTransferLabels[static_cast<size_t>(transferKind(t))] : nullptr;
}

/// WHAT THE MENU AND THE NOTIFIER CALL IT, or "" for a code that is not a curve.
[[nodiscard]] inline const char* transferName(Transfer t) noexcept {
	return transferValid(t) ? kTransferNames[static_cast<size_t>(transferKind(t))] : "";
}

/// THE KIND A LABEL NAMES, AT ITS DEFAULT COEFFICIENT, or nothing for a label this build does not know. A file's
/// coefficient is applied after, by `transferWithCoefficient`.
[[nodiscard]] inline std::optional<Transfer> transferFromLabel(const char* label, size_t length) noexcept {
	for (size_t i = 0; i < kTransferCount; ++i) {
		if (std::strlen(kTransferLabels[i]) == length && std::memcmp(kTransferLabels[i], label, length) == 0) {
			return transferDefaultOf(static_cast<Transfer>(i));
		}
	}
	return std::nullopt;
}

} // namespace feathers::device
