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

#include <algorithm>
#include <array>
#include <cstdint>

// **THE ADDITIVE ENGINE'S VOCODER, THE VOICE'S HALF** (additive engine design §9, backlog OE/OE2): the
// sidechain's envelope, analysed ONCE per synth (fx/spectral/spectral_sidechain.h), read by each voice AT ITS OWN PARTIALS'
// FREQUENCIES. The voice multiplies its partial targets by `vocodeGains` after the drawer's gains (OscVoice::controlPartials, step
// 4a), so Poly comes free and Flatten is exact: the sound's own shape is known partial by partial. OE2 (§9.8): the banded road (the
// Spectral effect's Spacing, Widths and Bandwidth, laid by dsp/vocode_bands.h at the partials), Stereo and Spread.
namespace feathers::spectral {

struct ShaperParams;

/// THE OPERATOR'S SETTINGS, per sound and blended (Synth::drawerFor): a voice operator, like Damp — read by OscVoice, not by
/// prepare()/gains(), and kept out of what GainCache keys on (ShaperParams::shaping).
struct VocodeOp {
	static constexpr float kGateOffDb = -96.0f; ///< at or below: no gate (the Spectral effect's kVocodeGateOffDb)
	bool on{false};
	float depth{1.0f};   ///< 0..2: 0 the voice untouched, 1 the classic vocoder, past 1 only the envelope's peaks
	float flatten{1.0f}; ///< 0..1: how much of the sound's own shape is taken out
	float gateDb{kGateOffDb}; ///< dBFS: a partial reading the envelope below this is closed
	float lowHz{20.0f};    ///< below, the envelope is held at its level here
	float highHz{20000.0f}; ///< above, likewise
	/// OE2 THE BANDED ROAD (the Spectral effect's §10.7, §10.9): Bandwidth below 1 imposes the envelope through discrete Gaussian
	/// bands; the Spacing zone (0 Even, 1 Speech, 2 Low, 3 High, 4 Harmonic, 5 Inharmonic, 6 Free; engine-wide) places them, read by
	/// its fenced amount or Free's coordinate; the Widths zone (0 Even .. 5 Free; engine-wide) spreads their widths likewise.
	float bandwidth{1.0f};
	int zone{0};
	float zoneAmount{0.5f};
	float zoneFree{0.5f};
	int widthZone{0};
	float widthAmount{0.5f};
	float widthFree{0.5f};
	/// OE2 STEREO (engine-wide): 0 Mono (one envelope, both ears), 1 Stereo (each ear by its own sidechain channel), 2 Spread (one
	/// envelope, its bands alternating between the ears).
	int stereo{0};
	/// What the Spacing zone reads: nothing in Even, a named zone's fenced amount, Free's coordinate (0 is Even there too).
	[[nodiscard]] double zoneDrive() const noexcept {
		return zone <= 0 ? 0.0 : zone >= 6 ? static_cast<double>(zoneFree) : std::clamp(static_cast<double>(zoneAmount), 0.0, 1.0);
	}
	[[nodiscard]] double widthDrive() const noexcept {
		return widthZone <= 0 ? 0.0
		       : widthZone >= 5 ? static_cast<double>(widthFree)
		                        : std::clamp(static_cast<double>(widthAmount), 0.0, 1.0);
	}
	/// Whether the banded road is taken: Bandwidth below 100%, or a Spacing or Widths zone that moves the layout.
	[[nodiscard]] bool banded() const noexcept { return bandwidth < 0.999f || zoneDrive() != 0.0 || widthDrive() != 0.0; }
	/// Whether the ears differ (Stereo or Spread): the voice renders two ears for it.
	[[nodiscard]] bool stereoEars() const noexcept { return on && stereo != 0; }
	friend bool operator==(const VocodeOp&, const VocodeOp&) = default;
};

/// **THE SIDECHAIN'S ENVELOPE AS THE VOICES READ IT**, written by the synth once a chunk from its analyser, read by every voice
/// through a pointer (OscVoice::setVocodeEnvelope). The Spectral effect's grid (fx/spectral/spectral_envelope.h): kPoints log-spaced
/// points, 20 Hz to 20 kHz, here in dBFS (a full-scale sine at a bin centre reads 0) and with the analysis' bin spacing beside it,
/// which the calibration reads (§9.2). `db` is the mono sum's, or the LEFT channel's while `stereo`; `dbRight` the right channel's
/// then.
struct VocodeEnvelope {
	static constexpr int kPoints = 128;
	static constexpr double kLowHz = 20.0;
	static constexpr double kOctaves = 10.0;
	/// At or below: silence (the estimator's −120 dB floor, carried into dBFS with room to spare).
	static constexpr float kSilentDb = -150.0f;
	std::array<float, kPoints> db{};
	std::array<float, kPoints> dbRight{};
	bool stereo{false};
	double binHz{100.0};
	/// The analysis' Bands (engine-wide): the banded road lays this many.
	double bands{24.0};
	/// FREEZE'S KEY TRACK (§9.3): while `keyTrack`, each voice reads the held envelope at f · frozenHz / f0, so its formants move
	/// with its own note; `frozenHz` is the note's pitch when Freeze took hold.
	bool keyTrack{false};
	double frozenHz{261.6255653005986};
	/// Bumped whenever the contents change (the synth's sketch re-post and every voice's gain memory read it).
	uint64_t serial{0};
	VocodeEnvelope() {
		db.fill(kSilentDb);
		dbRight.fill(kSilentDb);
	}
	/// Ear `ear`'s grid under stereo mode `mode`: the right channel's only for ear 1 under Stereo with a stereo analysis.
	[[nodiscard]] const std::array<float, kPoints>& gridFor(int ear, int mode) const noexcept {
		return ear == 1 && mode == 1 && stereo ? dbRight : db;
	}
};

/// THE CALIBRATION (§9.2): a line spectrum at spacing f0 carries the power of a noise whose envelope reads `dbfs` on bins of `binHz`
/// when each line is (2/π)·sqrt(2·f0/binHz) times the envelope as an amplitude, with the dB mean's bias on noise (2.507 dB) put back.
inline constexpr double kVocodeNoiseBiasDb = 2.5066;

/// **THE BATCH**: gains for partials [first, count) into `gain` (partials below `first` are written 1: the sub under Sub priority,
/// which the operator leaves alone), for ear `ear` (0 L, 1 R). `hz` the sounding frequencies (rising), `formant` Formant Shift's
/// ratios at them or null, `drawer` the voice's drawer (the banded road reads its Formant Shift at the band centres), `shape`
/// the terrain's amplitude before the drawer, `fade` the Nyquist fade, `f0Hz` the note. The mean shape is taken here, over the
/// partials it vocodes. No allocation.
void vocodeGains(const VocodeOp& op, const VocodeEnvelope& env, int ear, const float* hz, const float* formant,
                 const ShaperParams& drawer, const float* shape, const float* fade, int first, int count, double f0Hz,
                 float* gain) noexcept;

} // namespace feathers::spectral
