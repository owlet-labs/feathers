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

#include "dsp/spectral_shaper.h"
#include "dsp/constellation.h"
#include "dsp/vocode.h"
#include "device/state_blocks.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace feathers {

/// **THE SPECTRAL DRAWER'S STORED STATE** (additive design §3.1, C6) — what the drawer holds that is not parameters:
///   - THE MASTER SWITCH (`spectral_on`), ONE FOR THE WHOLE DRAWER (lead's call, 2026-10-10, revisable): a toggle
///     to disable spectral processing, an A/B on the tab. Not automatable, so patch state beside the
///     parameters. Default ON: a fresh drawer is neutral, so on costs nothing audible; off is the engine's own road to the
///     bit with every sound's settings kept. A per-sound "off" is that sound's operators switched off.
///   - EACH SOUND'S DRAWN CURVE, 64 harmonic bands and its own switch (per sound, as every drawer setting is since the
///     decision of 2026-10-10). Not params (C6): 64 automation lanes a sound would be noise. Blended by weight with the
///     rest of the drawer (Synth::drawerFor).
/// Saved as the keyed `spectral` block (audition.cpp, state_block_order.h), each value under its key below, and carried
/// to the engine on process()'s schedule (`Synth::setSpectralState`), like the sound gammas.
struct SpectralState {
	bool on = true;
	std::array<spectral::DrawnCurve, kMaxSounds> curve {};
	/// **OE: THE ENGINE VOCODER'S FROZEN ENVELOPE AS LOADED** (additive design §9.3; the Spectral effect's Z10b road): the words a
	/// patch carried under `vocode.held`, and a count of the loads that carried them, which the synth reads to prime its held
	/// envelope ONCE per load. A SAVE does not read these: it reads the synth's own snapshot (Synth::readVocodeFreeze), which holds
	/// what the audio thread froze since.
	std::array<float, 526> vocodeFreeze {};
	uint32_t vocodeFreezeLoads = 0;

	friend bool operator==(const SpectralState&, const SpectralState&) = default;
};

/// THE KEYS a patch stores the state under (`device::validStateKey`'s alphabet), and the float count each carries —
/// tests/state_schema.ledger names each, as it names a device's extra state. A sound's curve is under its letter.
inline constexpr const char* kSpectralOnKey = "spectral_on";
/// A drawn band's gain is >= 0 (spectral_shaper.h) and the hand draws up to +12 dB: the ONE bound the editor's edit
/// (ParamBridge::setSpectralCurveBand) and the loader (loadSpectralState) both pin a band to.
inline constexpr float kSpectralCurveGainMax = 4.0f;
inline constexpr size_t kSpectralOnFloats = 1;
inline constexpr size_t kSpectralCurveOnFloats = 1;
inline constexpr size_t kSpectralCurveGainFloats = spectral::kCurveBands;
/// OE/OE2: the frozen envelope's block (Synth::publishVocodeFreeze): holding, the bin spacing (a double), the note it was caught at (a
/// double), whether it is a stereo pair, each channel's flatness above 2 kHz (Sibilance reads it; a float each), then the left (or
/// mono) grid's and the right grid's kPoints floats in dBFS, each by its
/// bits — 16-bit words (device::BlockWriter). OE2 (2026-10-27): the right channel joined, a new layout, so a new key (`vocode.freeze`
/// is retired, kCoreRetiredKeys).
inline constexpr const char* kVocodeFreezeKey = "vocode.held";
inline constexpr size_t kVocodeFreezeFloats = 1 + 4 + 4 + 1 + 2 + 2 + 2 * 128 + 2 * 128;
static_assert(std::tuple_size_v<decltype(SpectralState::vocodeFreeze)> == kVocodeFreezeFloats);
/// `a.spectral.curve.on` .. `h.spectral.curve.on`, and the gains the same way.
inline constexpr std::array<const char*, kMaxSounds> kSpectralCurveOnKeys{
    "a.spectral.curve.on", "b.spectral.curve.on", "c.spectral.curve.on", "d.spectral.curve.on",
    "e.spectral.curve.on", "f.spectral.curve.on", "g.spectral.curve.on", "h.spectral.curve.on"};
inline constexpr std::array<const char*, kMaxSounds> kSpectralCurveGainKeys{
    "a.spectral.curve.gain", "b.spectral.curve.gain", "c.spectral.curve.gain", "d.spectral.curve.gain",
    "e.spectral.curve.gain", "f.spectral.curve.gain", "g.spectral.curve.gain", "h.spectral.curve.gain"};

/// OE: THE FROZEN ENVELOPE'S BLOCK, decoded — whether it holds, the bin spacing it was measured at, the note's pitch it was
/// caught at, whether it is a stereo pair, and the grids in dBFS (the right is the left's when not).
struct VocodeFreezeBlock {
	bool holding = false;
	double binHz = 100.0;
	double frozenHz = 261.6255653005986;
	bool stereo = false;
	float flatness = 0.0f;
	float flatnessRight = 0.0f;
	std::array<float, spectral::VocodeEnvelope::kPoints> grid {};
	std::array<float, spectral::VocodeEnvelope::kPoints> gridRight {};
};
inline void writeVocodeFreeze(const VocodeFreezeBlock& b, float* out) noexcept {
	device::BlockWriter w(out, kVocodeFreezeFloats);
	w.flag(b.holding);
	w.f64(b.binHz);
	w.f64(b.frozenHz);
	w.flag(b.stereo);
	w.f32(b.flatness);
	w.f32(b.flatnessRight);
	for (const float g : b.grid) {
		w.f32(g);
	}
	for (const float g : b.gridRight) {
		w.f32(g);
	}
}
/// False for a block not this layout's, or one holding a non-finite value: the caller plays as if nothing were stored.
inline bool readVocodeFreezeBlock(const float* in, size_t n, VocodeFreezeBlock* out) noexcept {
	if (n != kVocodeFreezeFloats) {
		return false;
	}
	device::BlockReader r(in, n);
	VocodeFreezeBlock b;
	b.holding = r.flag();
	b.binHz = r.f64();
	b.frozenHz = r.f64();
	b.stereo = r.flag();
	b.flatness = r.f32();
	b.flatnessRight = r.f32();
	if (!(b.flatness >= 0.0f && b.flatness <= 1.0f) || !(b.flatnessRight >= 0.0f && b.flatnessRight <= 1.0f)) {
		return false;
	}
	for (float& g : b.grid) {
		g = r.f32();
		if (!std::isfinite(g)) {
			return false;
		}
	}
	for (float& g : b.gridRight) {
		g = r.f32();
		if (!std::isfinite(g)) {
			return false;
		}
	}
	if (!r.ok() || !(b.binHz > 0.0 && b.binHz < 1e6) || !(b.frozenHz > 0.0 && b.frozenHz < 1e6)) {
		return false;
	}
	*out = b;
	return true;
}

} // namespace feathers
