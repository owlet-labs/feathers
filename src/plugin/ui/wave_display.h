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

#include "dsp/character_bank.h"
#include "dsp/vox_bake.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/widget_registry.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <visage/ui.h>

// The mock's `#wave`: one cycle of the sound you have dialled in, in the footer's left third.
//
// WHAT IT DRAWS, and this is worth being exact about because the plan's own summary of this task
// says otherwise. It is NOT an oscilloscope of the audio output. The mock's `drawWave` evaluates
// `evalWave(x/600, blended())` — the blended WAVETABLE, one cycle of it — and redraws only when the
// character inputs change (`waveInputsKey`), which is not something a scope would do. So this is a
// picture of the instrument's voice rather than of its signal, and that is the better instrument:
// an output scope is empty whenever nobody is holding a note, while this says what the sound IS
// whether or not it is sounding.
//
// The consequence is that no audio-thread tap is needed at all, and none is taken. There is no ring
// buffer, no atomic reader gate, and nothing whatsoever added to the render path — the display is
// derived from the parameters on the main thread, so it costs exactly nothing when no editor is
// open. A true output scope remains a perfectly good separate feature; it is simply not this one.
//
// THE CURVE IS THE ENGINE'S OWN. It is built by the same two calls the audio thread makes —
// buildPhiMorphWavetable/buildPhiStairParams, crossfaded by Path with the same helpers the
// CharacterBank uses — and read through the same evaluators. The parameters reach it through
// characterFromParams(), which the synth also uses, so there is one mapping rather than two readings
// that could drift. A drawing of the waveform that came from a second model of the engine would be a
// lie that looked like a design.

namespace feathers::ui {

class WaveDisplay : public visage::Frame {
public:
	WaveDisplay(ParamBridge& bridge, WidgetRegistry& registry);

	void draw(visage::Canvas& canvas) override;

	/// The mock's `#wave{width:300px;height:74px}`.
	static constexpr float kWidth = 300.0f;
	static constexpr float kHeight = 74.0f;

	/// How finely the cycle is sampled, as points per LOGICAL pixel of width. The mock walks a 600px
	/// backing for a 300px display — two samples per displayed pixel — and undersampling a waypoint
	/// waveform does not soften it, it moves the corners. Multiplied by the display scale too, since a
	/// logical pixel is more than one real one on a scaled editor.
	static constexpr float kPointsPerPixel = 2.0f;

	/// Never fewer than the mock's own 601, however small the box gets.
	static constexpr size_t kMinPoints = 601;

	/// The mock's own vertical scale, in its logical units: the centre line at half height, and a
	/// full-scale sample reaching `kAmplitude` from it. Its clamp of ±1.25 is what keeps a hot
	/// waypoint inside the box.
	static constexpr float kAmplitude = 29.0f;
	static constexpr float kClamp = 1.25f;

	/// How many colour steps the tint ramp is quantised into across the width.
	///
	/// Not a visual decision — the ramp is smooth to the eye either way at this size — but a cost one.
	/// Every distinct colour is a separate entry in visage's gradient atlas, and the curve is redrawn
	/// on every Path change, so a per-sample colour meant ~600 atlas entries per redraw and something
	/// like a hundred thousand during a few seconds of dragging. Two dozen bands is a ~25x cut for a
	/// brightness step of about 2%, which is under what a one-pixel line can show.
	static constexpr int kColourBands = 24;

	/// The mock draws its type into a 2x backing and displays it at half, so its `9px` is 4.5 of ours,
	/// and its fillText has no letter-spacing.
	static constexpr float kLabelSize = 4.5f;

	/// THE PITCH THIS SURFACE SINGS A VOWEL AT — kVoxPictureKey, which is C3 and which the terrain
	/// draws at too. The reasoning lives with the constant in vox_bake.h, beside the paragraph about
	/// why a vowel bake needs a note at all; named here so the scope's own callers can see it.
	///
	/// THE PICTURE IS THEREFORE PITCH-NOMINAL WHERE THERE IS A VOWEL, and honestly so: play far from
	/// C3 and the vowel you hear is flatter or sharper-resolved than the one drawn. That is a real
	/// limit of drawing a note-dependent cycle without a note, and it is stated rather than hidden.
	/// Everywhere with no vowel in it — most of the terrain, by the species duty — the curve is the
	/// bits it always was.
	static constexpr double kVoxRenderKey = kVoxPictureKey;

	/// How excited the morph is: 0 parked at either sound, 1 at the midpoint. THE ENGINE'S OWN
	/// quantity, not a UI one — `morphExcite = cf * cfInv * 4` in morph_renderer.cpp:96 — and it
	/// really does drive amplitude overshoot, curvature boost and phase distortion while you are
	/// between the two sounds. Zero for the stair engine, which has no such factor at all: its
	/// crossfade is a plain interpolation of parameters.
	float excite() const;

	/// The sampled cycle, so a test can ask what shape is drawn rather than read pixels.
	std::span<const float> curve() const;

	/// The character the curve was built from — what the engine is running on right now.
	const Character& character() const noexcept { return built_; }
	/// HOW MANY PARTIALS THE ADDITIVE CURVE SUMS at `points` samples: what the curve's own sampling resolves without
	/// folding (a quarter of the points, Basic Shapes' rule for its mip), at most every partial there is.
	static int partialsDrawnAt(size_t points);

	/// Rebuild the curve if the character has moved since it was last built. Called by draw(), and
	/// public so a test can drive it without a canvas. Returns true when the curve changed.
	bool refresh();

	/// What the ramp's ends are painted in, so a case can ask the scope the same question it asks the
	/// rail rather than read colours back out of a canvas.
	uint32_t seedHueForTest(size_t slot) const { return seedHue(slot); }

	/// THE CHARACTER THIS PICTURE IS OF, exposed for the one case that can hold the watch list
	/// honest.
	///
	/// The list in the constructor is a hand-kept subset of what characterNow() reads, and the
	/// comment beside it says so — it has already gone stale twice, once on drift and once on
	/// Smoothness and FM Depth. A case can only DERIVE that list by driving every parameter and
	/// asking whether the character moved, which means it has to be able to ask.
	Character characterNowForTest() const { return characterNow(); }

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	/// The colour the sound in `slot` is wearing. The ramp runs between the first two.
	uint32_t seedHue(size_t slot) const;

	Character characterNow() const;
	/// The likes' phase warps the scope's character is built through, filtered to the engine drawn.
	mutable std::array<LikeWarp, kMaxSounds> likeTable_ {};
	size_t sampleCount() const;

	ParamBridge& bridge_;
	std::vector<float> curve_;
	size_t builtPoints_ = 0; ///< what the curve was sampled at, so a resize rebuilds it
	Character built_ {};
	/// The Resonate string's model the curve was drawn for — not on the Character, so compared beside it.
	WeaveModel builtModel_ = WeaveModel::Ring;
	/// And Resonate's Stiffness and Coupling the curve was drawn under, compared beside it for the same reason.
	ResonateShaping builtShaping_ {};
	bool everBuilt_ = false;
	/// THE ADDITIVE BUILD'S WORKING SPECTRA AND ITS RESULT (2026-10-10): the bank's own buildPartialsFor, called on demand
	/// like every other engine's build here. On the heap, the scratch being eight spectra; made at the first Additive draw.
	std::unique_ptr<PartialsScratch> partialsScratch_;
	partials::PartialSpectrum partialsSpectrum_ {};
};

} // namespace feathers::ui
