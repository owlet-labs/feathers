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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/wave_display.h"

#include "dsp/flagship.h"
#include "dsp/ks_picture.h"
#include "dsp/morph_renderer.h"
#include "dsp/osc_voice.h"
#include "dsp/slice_build.h"
#include "dsp/stair_renderer.h"
#include "dsp/vox_bake.h"
#include "plugin/character_params.h"
#include "plugin/sound_hues.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace feathers::ui {

namespace {

/// The mock's `#wave{background:#030509}`. A literal rather than a token because the mock makes it
/// one: the scope is its own instrument, darker than any panel, so the curve has somewhere to glow.
constexpr uint32_t kScopeGround = 0xff030509;
constexpr float kCorner = 8.0f; ///< the mock's `border-radius:8px`

/// The corner the glitch keeps being photographed in, in window pixels. The same 100 the batcher's
/// origin tripwire uses, deliberately: two instruments reporting on one region want one number, so a
/// line from either can be read against the other without arithmetic.
constexpr float kCornerWatch = 100.0f;


/// Every parameter the drawn cycle depends on that is not one of the sounds'. Watched so that
/// automation, a preset load or the host's own generic panel redraws the scope, not just our knobs.
///
/// The sounds' own forty-eight are added beside these, by the same rule that names them, rather than
/// listed: a scope that watched six of eight sounds would go stale on the two nobody thought to type
/// out, and it would go stale silently.
constexpr ParamId kInputs[] = { ParamId::OscType, ParamId::Path, ParamId::GlobalGamma,
	                            // Travel moves the character as surely as zone does, so the picture
	                            // has to follow it. They were absent while they were dormant.
	                            ParamId::PosX, ParamId::PosY,
	                            // Weather scales travel, so it moves the picture as surely as travel
	                            // does — and at zero it is what makes the picture v1's again.
	                            ParamId::Weather,
	                            // The warp SHAPE is engine-wide (an enum cannot be blended), so it
	                            // belongs on this list and not in the per-sound loop below.
	                            ParamId::WarpMode,
	                            // And the Resonate string's model, engine-wide for the same reason: it is not on the
	                            // Character at all, so refresh() compares it beside the character.
	                            ParamId::ResonateModel,
	                            // And the two Resonate shapers a rest pose depends on.
	                            ParamId::ResonateStiffness, ParamId::ResonateCoupling,
	                            // And the Basic Shapes mix, engine-wide and on the Character.
	                            ParamId::ShapesSine, ParamId::ShapesTriangle, ParamId::ShapesSaw,
	                            ParamId::ShapesSquare,
	                            // And the additive Shape, engine-wide and on the Character (2026-10-10).
	                            ParamId::AdditiveShape,
	                            // And A-PH's phasing, engine-wide and on the Character (2026-10-18).
	                            ParamId::AdditivePhases, ParamId::AdditiveSub, ParamId::AdditiveCeiling };

constexpr SoundField kSoundInputs[] = { SoundField::Zone,   SoundField::Offset, SoundField::Gamma,
	                                    SoundField::PlaceX, SoundField::PlaceY, SoundField::Enable };

} // namespace

WaveDisplay::WaveDisplay(ParamBridge& bridge, WidgetRegistry& registry) : bridge_(bridge) {
	for (const ParamId id : kInputs) {
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), this));
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (const SoundField field : kSoundInputs) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(soundParam(slot, field)), this));
		}
		// AND THE TWO PER-SOUND SCALARS THAT ARE NOT SoundFields. Smoothness (+5) and FM Depth (+4) sit
		// in the same 0x0200 placement block but outside the SoundField enum, so the loop above cannot
		// reach them and they were simply missing: characterFromParams reads both, the engine and the
		// terrain both moved when they were turned, and the scope alone did not: terrain and audible
		// waveform updated, the oscilloscope did not.
		//
		// THIS LIST IS A HAND-KEPT SUBSET OF WHAT characterNow() READS, which is the hazard rather than
		// the fix — the same shape as the drift characterNow's own comment records, where the scope
		// drew a sound with no drift on nobody's plane. Anything added to characterFromParams has to
		// be added here too, or the picture quietly stops being of the sound.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(smoothnessParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(fmDepthParam(slot)), this));
		// AND THE FIVE FIELD MODIFIER KNOBS (2026-08-25, +6..+A), which characterFromParams reads onto
		// SoundCoord beside those two. Added WITH the parameters rather than after someone noticed the
		// scope had stopped moving, which is how the two above arrived — the paragraph over this list
		// predicts exactly that failure and this is the first chance anyone has had to not repeat it.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(quantizeParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(syncParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(feedbackParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(pointsParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(gritParam(slot)), this));
		// WARP DEPTH — the scope draws the cycle and warp bends the cycle, so an unwatched knob is a
		// picture that stops agreeing with the sound the moment a hand turns it. The engine-wide
		// mode is watched below, once, rather than eight times.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(warpParam(slot)), this));
		// AND THE ADDITIVE SHAPE AMOUNT AND SIX BIAS KNOBS (2026-10-10), which characterFromParams reads onto SoundCoord
		// on Additive — the paragraph above predicts what leaving them off does.
		for (const ParamId id : { shapeAmountParam(slot), additiveRolloffParam(slot), additiveFormantParam(slot),
		                          additiveOddEvenParam(slot), additiveCombParam(slot), additiveDetailParam(slot),
		                          additiveOffGridParam(slot) }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id), this));
		}
	}
}

Character WaveDisplay::characterNow() const {
	// Modulated, not base: the picture is of the sound being played, so it follows host modulation
	// exactly as the engine does. Same mapping the synth uses — see character_params.h.
	//
	// AND THE TERRAIN NEXT DOOR READS THE OPPOSITE ROAD, DELIBERATELY. ExploreStage::askForTerrain
	// assembles its window from BASE values, because that window is a map of a PLACE rather than a
	// picture of a moment — modulation moves where you are standing, not what the country looks like
	// — and because the window is a content key, so reading modulated there would rebuild a 128x128
	// map every modulated block. See the note above askForTerrain.
	//
	// Two adjacent widgets on opposite roads is the sort of thing that reads as an inconsistency to
	// fix. It is not. Both files say so now, and a case pins each.
	const auto read = [this](ParamId id) { return bridge_.modulated(static_cast<clap_id>(id)); };
	// EVERY CHANNEL THE ENGINE READS, and the list is not optional. `characterFromParams` once took drift
	// and per-sound planes with defaults, and defaults are exactly what this call site was getting: the
	// scope drew a sound with no drift on nobody's plane while the engine played a drifted sound on its
	// own — measured at travelX 0.84 against the scope's 0.47. A picture of the instrument that is a
	// picture of a different instrument is worse than no picture, because it looks authoritative.
	//
	// The orientation off the same channel the engine reads, so a live rotation sweep redraws the
	// waveform it is actually producing rather than the one it would produce on the old plane.
	// The velocity off the same channel the engine publishes it on, so a picture drawn mid-gesture is
	// of the sound being made rather than of the one that would be made standing still.
	// And gamma's unbounded base off the same channel, for the reason every other argument on this
	// line is here: this call had a DEFAULT for it, and a default is how a scope comes to draw a
	// plausible neighbour of the sound being played. That is G1 exactly — the scope drew a sound with
	// no drift on nobody's plane, looked authoritative, and poisoned a round of listening judgments.
	// A sound carried along its line by the corner is somewhere else, and this is where the picture
	// finds out.
	Character c = characterFromParams(read, oscTypeFromParams(read), bridge_.tuning(), bridge_.puckVelocity(),
	                                  // WHERE EACH SOUND IS PLAYED, not where it was put: its Radius/Theta offset
	                                  // and the engine's live pull on it, through the engine's own function.
	                                  playedTravel(bridge_.soundTravel(), bridge_.travelPulls()),
	                                  bridge_.soundOrientations(),
	                                  bridge_.soundResidue(), bridge_.soundGamma(),
	                                  // AND WHAT IS ACTUALLY SOUNDING. While a preview is latched the engine is
	                                  // playing one sound stood on its own anchor, and a scope still drawing the
	                                  // blend would be the exact failure this feature is not allowed to have: a
	                                  // picture of a sound nobody is hearing.
	                                  bridge_.preview(),
	                                  // THE HEAD, NOT AN EAR. Terrain stereo evaluates two displaced characters
	                                  // in the synth; the scope draws the head-centre sound, which is the mono
	                                  // sound and both ears at width 0 — a per-ear picture would be two pictures.
	                                  EarSide::Head);
	// AND EACH SOUND'S LIKES' PHASE WARP, as the synth attaches them, so the scope draws what plays.
	const OscType engine = oscTypeFromParams(read);
	bool any = false;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		likeTable_[slot] = bridge_.likeEngineOf(slot) == static_cast<uint8_t>(engine) ? bridge_.likeWarpOf(slot) : LikeWarp {};
		any = any || !likeTable_[slot].identity();
	}
	if (any) {
		c.likes = { &likeTable_, bridge_.likeSerial() + 1 };
	}
	return c;
}

size_t WaveDisplay::sampleCount() const {
	// Two per logical pixel, and per DEVICE pixel on a scaled editor — the mock's 601-over-600 ratio
	// applied to whatever width we are actually given.
	const float scale = std::max(1.0f, dpiScale());
	const auto wanted = static_cast<size_t>(std::lround(width() * scale * kPointsPerPixel));
	return std::max(kMinPoints, wanted);
}

float WaveDisplay::excite() const {
	// The engine's own definitions, from morph_renderer.h — not a second copy of either formula. WHICH
	// of the two is the same question the BUILD asks, and it has to be asked the same way: the crossfade
	// only runs in parity mode now, so that is the only place its excitation is what you are hearing.
	//
	// This branch went stale the day §2c made the rule uniform. It read `count == 2` alone, which was
	// right until two sounds stopped taking the crossfade, and then quietly printed 4t(1−t) for a blend
	// — 1.000 against the 0.761 the engine was applying, at any puck off the tour. The test passed
	// because its fixture sat on the one point where the two formulas agree.
	if (built_.engine != OscEngine::Morph) {
		return 0.0f;
	}
	if (built_.count == 2 && built_.v1Crossfade) {
		return morphExcite(built_.xf);
	}
	double weights[kMaxSounds] = {};
	const size_t n = std::min<size_t>(built_.count, kMaxSounds);
	for (size_t i = 0; i < n; ++i) {
		weights[i] = built_.sounds[i].weight;
	}
	return blendExcite({ weights, n });
}

bool WaveDisplay::refresh() {
	const Character now = characterNow();
	const size_t points = sampleCount();
	// The model read ONE way across the editor (bridge_.value, as the Explore stage reads it): it is stepped
	// and not modulatable, so there is no modulated reading to prefer.
	const WeaveModel model = static_cast<WeaveModel>(steppedIndex(
	    bridge_.value(static_cast<clap_id>(ParamId::ResonateModel)), kResonateModelNames.size()));
	// AND RESONATE'S SHAPING, the part of it a rest pose depends on: the springs home (Stiffness) and the
	// neighbour springs (Coupling) — the pose is the solve of k(x - home) = c laplacian(x). Modulated, as the
	// character is: the picture is of the sound being played.
	ResonateShaping shaping;
	shaping.stiffness = static_cast<float>(std::exp2(bridge_.modulated(static_cast<clap_id>(ParamId::ResonateStiffness))));
	shaping.coupling = static_cast<float>(std::exp2(bridge_.modulated(static_cast<clap_id>(ParamId::ResonateCoupling))));
	if (everBuilt_ && now == built_ && points == builtPoints_ && model == builtModel_ && shaping == builtShaping_) {
		return false;
	}
	built_ = now;
	builtModel_ = model;
	builtShaping_ = shaping;
	builtPoints_ = points;
	everBuilt_ = true;
	curve_.resize(points);

	// THE BANK'S OWN BUILD, called rather than restated: buildMorphFor and buildStairFor are what
	// CharacterBank::build runs, so the curve is the engine's table and not a model of it — including
	// which blend rule applies at this sound count, which is exactly the kind of thing a second copy
	// would get right for a release and then quietly stop getting right. A build is ~0.4µs per sound;
	// the sampling below dominates, and both together are far inside a frame, which is why this can be
	// done on demand rather than cached against a key the way the mock does it.
	if (built_.engine == OscEngine::Morph && built_.flagship) {
		// The flagship through the bank's own build again: buildFlagshipFor is what
		// CharacterBank::build runs for a V5 character, so the curve carries the operators the
		// voices are playing — sync, feedback and the staircase included. Drawn through
		// evalFlagship rather than the baked grid for the reason the stair branch gives below: the
		// grid's mip is chosen by the fundamental, and a picture has no fundamental. At an
		// identity-corner position WITH NO VOWEL IN IT this is evalMorphTable exactly, so the picture
		// degrades to the morph branch precisely where the sound does — the vowel is the one thing
		// that puts a cycle on the blended road without an operator having done it (V22c).
		FlagshipRecipe recipe = buildFlagshipFor(built_);

		// AND THE VOWEL, which this surface drew straight past until now: V22c puts the vox
		// crossfade at the operator chain's SOURCE READ, so in vowel country the cycle the voices
		// make is the blend and not the morph table underneath it. Neither picture knew that, and a
		// V5 sound with a throat drew as its vowel-free build — a waveform nobody was playing.
		//
		// One kilobyte of stack, on the UI thread, behind refresh()'s own change gate: this runs when
		// the character actually moved, not per frame. buildVoxFor costs one triangle per sound and
		// stops for the three quarters of the terrain with no vowel in it, so a vowel-free patch pays
		// what it always paid.
		const VoxCharacter vox = buildVoxFor(built_);
		VoxCycle cycle;
		if (vox.audible) {
			bakeVoxCycle(vox.params, pitchToHz(kVoxRenderKey), cycle);
		}
		const FlagshipSource source =
		    cycle.voiced ? FlagshipSource { &cycle, vox.presence } : FlagshipSource {};

		// THE PREPARE IS NOT OPTIONAL AND THE COMPILER WILL NOT SAY SO. FlagshipSource is a defaulted
		// parameter on both entries, so `buildFlagshipFor` followed by `evalFlagship(recipe, phase,
		// source)` compiles clean and is wrong: `held` is the sync+feedback chain sampled at the step
		// centres, and buildFlagshipFor sampled it on the vowel-free cycle. The final combine drags
		// the output back toward those held values, so the vowel would come out attenuated by
		// (1 − quantize) and vanish entirely at quantize 1 — a plausible picture, a half-applied
		// blend, nothing failing. Three statements, the voice's own spelling (osc_voice.cpp,
		// bakeVoxInto), and it re-derives nothing when the source is null.
		prepareFlagshipRecipe(recipe, source);
		for (size_t i = 0; i < points; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
			curve_[i] = std::clamp(evalFlagship(recipe, phase, source), -kClamp, kClamp);
		}
	}
	else if (built_.engine == OscEngine::Weave) {
		// THE STRING'S REST POSE, NOT ITS HOME SHAPE AND NOT THE LIVE RING. The voices scan ONE string
		// that is bowed, plucked and travelling, so no picture of a moment is "the sound"; what the
		// coordinate SELECTS is the pose the string comes to rest in under its laws — k(x - home) =
		// coupling's pull, solved (weaveRestCycle), at the loudness the AGC settles to — and the live
		// ring wanders around it. Drawn as the scan reads it: linear between the 32 path points, wrapped.
		float cycle[WeaveString::kNodes];
		WeaveCharacter laws = buildWeaveFor(built_);
		shapeWeaveLaws(builtShaping_, laws.a);
		shapeWeaveLaws(builtShaping_, laws.b);
		weaveRestCycle(builtModel_, laws, cycle);
		constexpr int kN = WeaveString::kNodes;
		for (size_t i = 0; i < points; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
			const double at = phase * static_cast<double>(kN);
			const int index = std::min(kN - 1, static_cast<int>(at));
			const auto frac = static_cast<float>(at - static_cast<double>(index));
			const float from = cycle[index];
			const float to = cycle[(index + 1) % kN];
			curve_[i] = std::clamp(from + (to - from) * frac, -kClamp, kClamp);
		}
	}
	else if (built_.engine == OscEngine::Shapes) {
		// THE ENGINE'S OWN DECODE: the bank's normalised blend through the bank's recipe's warp, read by
		// evalShapesWarped exactly as the voice reads it. FM is per note (the alias cap needs the fundamental)
		// and a picture has no note, so it is drawn without FM — as V5's picture is. The mip is the one whose
		// partials the curve's own sampling can resolve, so the picture does not alias either.
		warmShapeTables(); // a no-op wherever a bank exists, which is everywhere but a bare widget test
		FlagshipRecipe recipe = buildFlagshipFor(built_);
		const ShapesWeights weights = normaliseShapes(built_.shapes);
		const int mip = shapesMipFor(1.0, static_cast<double>(points) * 0.25);
		// AND THE OPERATOR TIER (backlog KV), prepared against this mip: `evalShapesCycle` is the voice's decode.
		prepareShapesRecipe(recipe, weights, mip);
		for (size_t i = 0; i < points; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
			curve_[i] = std::clamp(evalShapesCycle(recipe, weights, mip, phase), -kClamp, kClamp);
		}
	}
	else if (built_.engine == OscEngine::Partials) {
		// THE BLENDED SPECTRUM, SUMMED AS ONE CYCLE (plan Task 9b) — the bank's own build (buildPartialsFor, what
		// CharacterBank::buildOne runs for a Partials character), so the curve is the spectrum the voices render and not a
		// model of it; NEVER the stair grid the last branch reads, and never silence. THE PHASES ARE NOT THE VOICE'S: a voice's
		// note-on puts its partials at SCHROEDER'S phases weighted by that note's own spectrum (PartialBank::reset(power,
		// count)), which depend on the pitch and the note, and a picture has neither. So the curve uses Newman's flat phases
		// (Schroeder's equal-power case) over the partials it sums: the same magnitudes the voice plays, a stand-in phase
		// law, and so a waveform that may differ in shape from any one note's while its spectrum is exact. It sums the
		// partials the curve's sampling can resolve, with no Nyquist fade (there is no pitch to fade at). partials::cycleOf
		// states the sum.
		if (partialsScratch_ == nullptr) {
			partialsScratch_ = std::make_unique<PartialsScratch>();
		}
		buildPartialsFor(built_, partialsSpectrum_, *partialsScratch_);
		const int drawn = partialsDrawnAt(points);
		partials::cycleOf(partialsSpectrum_, drawn, drawn, nullptr, static_cast<int>(points), static_cast<double>(points - 1),
		                  curve_.data());
		for (float& y : curve_) {
			y = std::clamp(y, -kClamp, kClamp);
		}
	}
	else if (built_.engine == OscEngine::String) {
		// THE STRING AS HEARD (KS design §2.3), the terrain's picture of it with every partial the curve resolves: the pluck's
		// tone (the bank's own buildPartialsFor) and the blended material (buildStringFieldsFor), each harmonic weighed by how
		// long the loop lets it ring, at C3 (ks::heardSpectrum). A picture of what the place gives a note, not one note's
		// decaying waveform, which moves for as long as it rings.
		if (partialsScratch_ == nullptr) {
			partialsScratch_ = std::make_unique<PartialsScratch>();
		}
		buildPartialsFor(built_, partialsSpectrum_, *partialsScratch_);
		const int drawn = partialsDrawnAt(points);
		partials::PartialSpectrum heard;
		ks::heardSpectrum(partialsSpectrum_, buildStringFieldsFor(built_), drawn, heard, {}, static_cast<double>(points) / 2.0 - 1.0);
		partials::cycleOf(heard, drawn, drawn, nullptr, static_cast<int>(points), static_cast<double>(points - 1), curve_.data());
		for (float& y : curve_) {
			y = std::clamp(y, -kClamp, kClamp);
		}
	}
	else if (built_.engine == OscEngine::Morph) {
		const owlet::dsp::PhiMorphWavetable table = buildMorphFor(built_);
		for (size_t i = 0; i < points; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
			curve_[i] = std::clamp(evalMorphTable(table, phase), -kClamp, kClamp);
		}
	}
	else {
		const StairTable table = buildStairTable(buildStairFor(built_));
		for (size_t i = 0; i < points; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
			// evalStairTable rather than the 128-slot grid the renderer plays: the grid's mip is chosen
			// by the fundamental, and a picture has no fundamental. This is the shape those slots are
			// built FROM — the ideal the band-limiting approximates — which is the honest thing to draw
			// when the note it would be played at is unknown.
			curve_[i] = std::clamp(evalStairTable(table, phase), -kClamp, kClamp);
		}
	}
	return true;
}

int WaveDisplay::partialsDrawnAt(size_t points) {
	return static_cast<int>(std::min<size_t>(static_cast<size_t>(partials::kMaxPartials), points / 4));
}

std::span<const float> WaveDisplay::curve() const {
	return { curve_.data(), curve_.size() };
}

uint32_t WaveDisplay::seedHue(size_t slot) const {
	// READ LIVE, NOT CACHED, and through the same `hueIndexOf` fold every other surface reads: the
	// ramp is recomputed on every draw anyway, and a colour copied at construction would be the
	// original bug with an extra frame in front of it.
	return kSeedHues[hueIndexOf(bridge_.soundHues(), slot)];
}

void WaveDisplay::draw(visage::Canvas& canvas) {
	refresh();

	const float w = width();
	const float h = height();
	const size_t points = curve_.size();
	if (points < 2) {
		return;
	}

	canvas.setColor(kScopeGround);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, kCorner);
	// A hairline around it, which the mock does NOT have — its `#wave` is a background and a radius
	// and nothing else, and rendered live it reads as frameless because #030509 on #010204 is not a
	// visible edge. Added because the scope wants its own frame, in the same kLine
	// the engine selector and the Engine rail's panels use, so it reads as a panel of the same family
	// rather than as a new idea.
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, kCorner, 1.0f);

	// The zero line, so a waveform sitting mostly above or below it reads as doing so.
	const float middle = h * 0.5f;
	canvas.setColor(withAlpha(kMuted, 0.15f));
	canvas.fill(0.0f, middle, w, 1.0f);

	// The mock tints the curve from sound A's hue to sound B's, and brightens whichever one the blend
	// currently favours: `.5 + .5*weight`. With two sounds the weights ARE the crossfade, so this is
	// Path itself — the curve leans toward the sound you are nearer.
	//
	// THE BRIGHTNESS IS COMPOSITED INTO THE COLOUR rather than left to alpha, and that is not a
	// refactor. A stroked polyline overdraws itself: 600 rounded capsules at half a pixel of overlap
	// each put roughly three layers on every pixel, and 1-(1-a)³ takes a 0.5 alpha to 0.875 while
	// leaving a 1.0 at 1.0. The mock's 2:1 brightening arrived on screen as about 7%, which is why
	// the tint read as flat. Blending against the scope's own ground at full alpha makes the ramp
	// survive whatever the overdraw does, and it halves the draw calls: the faint accent underlay
	// that stood in for the mock's shadow is gone with it, since an opaque stroke would bury it.
	const float path = std::clamp(static_cast<float>(built_.xf), 0.0f, 1.0f);
	const float weightA = 1.0f - path;
	const float weightB = path;

	const float scale = h / kHeight;
	const float amplitude = kAmplitude * scale;
	const auto pointAt = [&](size_t i) {
		const float x = w * static_cast<float>(i) / static_cast<float>(points - 1);
		return visage::Point { x, middle - curve_[i] * amplitude };
	};

	// WHERE THE SCOPE WATCHES ITSELF. Every point below is computed from an evaluated character rather
	// than placed from a clamped parameter, which makes this the one straight line in the editor that
	// can be handed a finite-but-wrong coordinate — and the at-rest flicker says the artifact being
	// hunted is redrawn every frame, which is what this loop does.
	//
	// Hoisted out of the loop because the widget's position cannot change inside one draw, and the
	// check has to cost nothing: it runs once per point, several hundred times a frame.
	const visage::Point scopeOrigin = positionInWindow();
	const bool scopeAwayFromCorner = scopeOrigin.x >= kCornerWatch || scopeOrigin.y >= kCornerWatch;
	const auto watch = [&](size_t i, const visage::Point& p) {
		using Fault = diagnostic::ScopeFault;
		const auto report = [&](Fault fault) {
			diagnostic::scopePoint(i, fault, p.x, p.y, scopeOrigin.x + p.x, scopeOrigin.y + p.y);
		};
		if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
			// Reported BEFORE the guard below refuses it. The guard is right — a non-finite point must
			// not reach the renderer — but it also means this value is destroyed one frame before the
			// batcher's WILD check could ever have named it, which is why that report has stayed silent.
			report(Fault::NonFinite);
		}
		else if (i > 0 && p.x == 0.0f && p.y == 0.0f) {
			// x is w*i/(points-1), so for any point after the first it is zero only when the widget has
			// no width. A point at the local origin is therefore never something this curve computes on
			// purpose, and it fans from the SCOPE's corner rather than the window's — which is precisely
			// the case the batcher's origin tripwire is built to exempt.
			report(Fault::Zeroed);
		}
		else if (scopeAwayFromCorner && scopeOrigin.x + p.x < kCornerWatch
		         && scopeOrigin.y + p.y < kCornerWatch) {
			report(Fault::WindowCorner);
		}
	};

	// The colour is quantised into bands and only pushed when the band changes: the ramp is smooth to
	// the eye at a 2% step, and a per-sample colour would put six hundred entries into visage's
	// gradient atlas on every redraw — which happens on every Path change, so a drag was pushing tens
	// of thousands of them a second. The band edges land on t=0 and t=1, so the ends, which are what
	// the brightening is measured on, are untouched by this.

	// THE ENDS ARE THE FIRST TWO SOUNDS' OWN COLOURS rather than the first two palette entries, which
	// is the whole of the fix: `kSeed1 -> kSeed2` named "sound A to sound B" correctly only while a
	// colour was a function of the row. Hoisted for the same reason the origin above is — neither end
	// can change inside one draw — so the reorder costs two reads a frame rather than one per band.
	const uint32_t rampFrom = seedHue(0);
	const uint32_t rampTo = seedHue(1);
	int lastBand = -1;
	for (size_t i = 1; i < points; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(points - 1);
		const int band = static_cast<int>(std::lround(t * (kColourBands - 1)));
		if (band != lastBand) {
			lastBand = band;
			const float banded = static_cast<float>(band) / static_cast<float>(kColourBands - 1);
			const uint32_t hue = blendColour(rampFrom, rampTo, banded);
			const float weight = weightA + (weightB - weightA) * banded;
			canvas.setColor(blendColour(kScopeGround, hue, 0.5f + 0.5f * weight));
		}
		const visage::Point from = pointAt(i - 1);
		const visage::Point to = pointAt(i);
		// Every point examined exactly once: `to` at each step, and `from` only on the first, where it
		// is the one point no later step will visit as its own `to`.
		if (i == 1) {
			watch(i - 1, from);
		}
		watch(i, to);
		// A FINITE CHECK, which this line did not have, and it is the one straight-line primitive in
		// the editor whose coordinates are COMPUTED rather than clamped: every other one is placed from
		// a sanitised parameter or a bounded geometry, and this one is placed from an evaluated
		// character. Found by auditing dashedLine's siblings after the reported fan of stray lines — a sample that came back
		// non-finite would hand the renderer a garbage quad here rather than being refused, and unlike
		// the dash walker there is no loop condition to stop it.
		if (!std::isfinite(from.y) || !std::isfinite(to.y)) {
			continue;
		}
		// Half the old width: the mock strokes 2px into a backing it displays at half size.
		canvas.segment(from.x, from.y, to.x, to.y, 1.0f * scale, true);
	}

	// The mock's two corner readings, at its own type size — 9px into a 2x backing, so 4.5 of ours,
	// and no letter-spacing, because its fillText has none.
	const visage::Font& face = font(kLabelSize * scale);
	canvas.setColor(withAlpha(kMuted, 0.9f));
	const float inset = 8.0f * scale;
	// The flagship reads off its flag rather than OscEngine — it renders on the morph path by
	// design, and a badge keyed on the render path alone would name the wrong instrument at the
	// audition (review-flagship-s2, LOW 1). "PHI V5" is the working name (assumption V1); the real
	// one lands at first sound.
	canvas.text(built_.flagship                     ? "PHI V5"
	            : built_.engine == OscEngine::Stair ? "PHI STAIR"
	            : built_.engine == OscEngine::Weave ? "PHI RESONATE"
	            : built_.engine == OscEngine::Shapes ? "BASIC SHAPES"
	            : built_.engine == OscEngine::Partials ? "ADDITIVE"
	            : built_.engine == OscEngine::String ? "PLUCK"
	                                                : "PHI MORPH",
	            face, visage::Font::kLeft, inset, inset * 0.5f, w, face.size() * 1.6f);

	// EXCITE, lower left, and it is the engine's own number rather than a UI one: morphExcite drives
	// amplitude overshoot, curvature boost and phase distortion, and it exists only while you are
	// between the two sounds. Drawn for the morph engine alone — the stair engine's crossfade is a
	// plain interpolation with no such factor, so printing one there would name a mechanism it does
	// not have. The notify readout floats over this corner while you are moving something, exactly as
	// it does in the mock.
	if (built_.engine == OscEngine::Morph) {
		char printed[32] = {};
		// The mock's own spelling: `excite 42%`, no space before the sign.
		std::snprintf(printed, sizeof(printed), "excite %.0f%%", excite() * 100.0f);
		canvas.text(printed, face, visage::Font::kLeft, inset, h - inset * 0.5f - face.size() * 1.6f, w,
		            face.size() * 1.6f);
	}
}

} // namespace feathers::ui
