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
#include "dsp/flagship.h"
#include "dsp/slice_basis.h"
#include "dsp/weave_models.h"
#include "plugin/params.h"
#include "plugin/ui/terrain_likes.h"
#include "plugin/ui/like_surface.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// The terrain: what the character layer looks like at a place, for every place in view.
//
// THE MAP IS A PURE FUNCTION OF POSITION, and that is the binding rule the whole file is arranged
// around (§4a of the phase-2 plan). The same place looks the same way in every session, at every
// zoom, under every pan, whatever else is on screen. There is NO normalisation over what is currently
// visible, no palette fitted to the window, no clustering recomputed per view — a colour that meant
// one thing before you zoomed and another after is a map that cannot be learned, and learning it is
// the entire point. Every transfer function below is a constant, and `terrainFeaturesAt` takes a
// position and nothing else.
//
// It is the third instance of the same rule: the SOUND is a pure function of position (home and
// travel), the ORIENTATION is a pure function of position (φ-derived from home), and now the PICTURE
// is too.
//
// THE VOWEL IS IN THESE COLOURS, and the one thing about it that is not a pure function of position
// is named rather than hidden: a vowel bake needs a fundamental, a picture has no note, so a V5
// texel in vowel country is drawn at a NOMINAL C3 (kVoxPictureKey) — the same pitch the scope sings
// at, so the two surfaces describe the same place the same way.
//
// The 32-sample, 12-harmonic lens below cannot resolve a vowel and folds part of it. That is
// measured rather than waved at: drawing the vowel misses the truth about those twelve harmonics by
// 0.0133 mean centroid against 0.0471 for omitting it — 3.5x closer, across 18 vowel regions in 6
// zones, every region improved. See the V5 branch in terrainFeaturesOn for the full figures.
//
// WHAT THE TWO AXES ARE, which is the honesty question this surface has to answer.
//
//   VERTICAL is HOME — `zone/1023 + offset + gamma + globalGamma` — the one coordinate a sound's
//   IDENTITY has. All four of those parameters address it; a zone step is 1/1023 of an offset step,
//   which is why there is no zone×offset plane to draw and why Explore waited for the engine.
//
//   HORIZONTAL is TRAVEL along the sound's own plane, and it is IDENTITY as much as home is. A sound
//   carries its own drift across that plane (`SoundTravel`), stored with the patch, added into the
//   decode beside whatever the puck contributes — so the total travel a texel is built at is a place
//   a sound can actually be, and the surface scrubs it. What the puck and Weather add on top is a
//   performance term the chart shows and never writes.
//
// A texel's character is built with the plane its OWN home gives it, not with the selected sound's.
// The two agree exactly where it matters: a chart point (h, t) means "if this sound's home were h,
// this is what it would be t along its plane", and a sound at home h has precisely the plane
// M(rotate, h) gives it. Deriving per texel is what makes the picture independent of which sound is
// selected, which is what "pure function of position" requires.

namespace feathers::ui {

/// A sound that has never had a rotation folded into it. Named so that a call site passing nothing can
/// be read as saying so, and so that the default argument below costs no zeroing of eighty-eight
/// doubles per texel.
inline const PlaneResidue kNoPlaneResidue {};

/// How the builders compute a home: `zone/1023 + offset + gamma`.
///
/// THE OTHER DIRECTION USED TO LIVE HERE — `homeToCoord`, which split a home back into the zone and
/// offset that reach it — and it is gone with the wall it was. See the note at its old site in
/// terrain.cpp: under the one-value design a scrub moves the sound's unbounded gamma base and the
/// two dials are bounded knobs summing on top, so nothing decomposes a home any more and nothing
/// clamps one.
double homeOf(double zone, double offset, double gammaSum);

/// The band of home the two DIALS can reach either side of wherever gamma has carried a sound.
///
/// It is no longer the edge of anything: the chart scrubs the base, which is unbounded, so this bounds
/// what a HOST can reach through automation rather than what a hand can reach through the glass. What
/// still rests on it is the widest zoom, which is sized to show the whole of it.
double homeFloor(double gammaSum);
double homeCeiling(double gammaSum);

//----------------//
// Span averaging //
//----------------//

/// HOW MANY POINTS A TEXEL IS BUILT FROM, given how much of the world it covers.
///
/// SPAN AVERAGING, over the texel's whole xy area rather than a single row. A texel is a CELL, not a point: at a wide zoom it covers
/// hundreds of zones of home and the matching width of travel, and drawing it from one sample in the
/// middle is a lucky guess about everything else it covers. Averaging what it covers turns
/// undersampling into honest blur.
///
/// WHY A SCHEDULE AND NOT A CONSTANT. Every sample is a built wavetable and a spectrum — a whole map is
/// 27ms at one sample a texel — so K is worth paying only where a cell is genuinely bigger than what is
/// being drawn. The finest feature is a φ triangle read at `phiFreq × rate` cycles of home
/// (phi_triangle.hpp:168), repeating every `1/(phiFreq × rate)`: measured worst case over the banks,
/// **169 zones for morph and 34.5 for STAIR**, whose glyph pattern (7 × φ³) is five times faster than
/// anything in morph. Below half of that a cell cannot alias and K is 1 — which is EVERY RUNG THIS
/// SURFACE SHIPPED WITH for morph, and all but the widest for stair. The change costs nothing where
/// anyone works today.
///
/// THE TIERS ARE THE MEASURED CONVERGENCE rather than a guess. A K-sample mean's error falls as 1/√K
/// and then stops mattering: at 384 zones a row the row-to-row step goes 27.0 → 15.4 at K≈4 → 14.1 at
/// 16 → 13.9 at 64, of 255. What remains is real variation between neighbouring intervals, not
/// sampling noise, so paying past sixteen buys nothing. Travel makes λ up to 2.5× finer than the
/// figures above — the rate rows retune as you move sideways — and the tiers are coarse enough to
/// absorb it.
int terrainSamplesFor(OscType engine, double cellZones);

/// Where sample `index` of `count` sits inside a texel, as a fraction of the cell in [-0.5, 0.5].
struct TerrainSampleOffset {
	double home = 0.0;
	double travel = 0.0;
};

/// THE FIRST SAMPLE IS ALWAYS THE CENTRE, which is what makes K=1 exactly the map this surface has
/// always drawn, and a first pass exactly today's picture at any K. The rest are a stratified n×n
/// lattice, each lattice row shifted along travel by the golden fraction of a sub-cell.
///
/// STRATIFIED RATHER THAN LOW-DISCREPANCY because K is tiny: with four or sixteen samples what matters
/// is that no quadrant goes unrepresented, which stratification guarantees and a sequence only
/// approaches. The φ shift is there because a plain lattice BEATS against a periodic field — every
/// sample landing on the same phase of a triangle is exactly the worst case — and an irrational offset
/// is the engine's own trick for that, turned on itself.
///
/// DETERMINISTIC AND STATELESS, no RNG anywhere: two builds of one window must be byte-identical or
/// half this suite's guarantees dissolve, and a random pattern would trade a visible artefact for an
/// invisible one.
TerrainSampleOffset terrainSampleOffset(int index, int count);

//----------//
// Features //
//----------//

/// What the engine IS at a point, measured from the waveform it would actually build there.
///
/// Measured rather than modelled: the character is built by the same call the audio thread makes and
/// evaluated through the same evaluator the scope draws with, then reduced to three numbers. A second
/// reading of the parameters would be a picture that can be subtly wrong for a whole release and look
/// like a design choice.
struct TerrainFeatures {
	/// RMS of one cycle. Drives luminance: bright is loud.
	float energy = 0.0f;
	/// Spectral centroid over the analysed band, 0 at the fundamental and 1 at the top harmonic.
	/// Drives hue: this is the "spectral tilt" channel.
	float centroid = 0.0f;
	/// The share of the magnitude spectrum above the halfway harmonic. Drives grain: this is
	/// curvature roughness measured where it shows up rather than where it is written.
	float roughness = 0.0f;
	/// THE ODD HARMONICS' SHARE of the magnitude spectrum — 1 is purely odd, 0 purely even, and a
	/// square wave sits near 1 where a full-spectrum saw sits near 0.55. The chosen second
	/// colour axis, and it says something neither of the two above can: they are both statements about
	/// spectral WEIGHT — where the energy sits and how much is up high — and neither notices parity. A
	/// hollow clarinet-ish tone and a full reedy one can share a centroid exactly.
	float parity = 0.0f;
	/// THE FUNDAMENTAL'S OWN SHARE of the magnitude spectrum: 1 is a pure tone, and it falls as the
	/// harmonics fill in. Distinct from the centroid because a big fundamental with a little sparkle on
	/// top reads MODERATE centroid and HIGH fundamental share, where an evenly-filled spectrum reads the
	/// same centroid and a low one.
	float fundamental = 0.0f;
};

/// How many samples of one cycle are analysed, and how many harmonics are measured.
///
/// A picture has no fundamental, so this is a cycle rather than a note: 32 points and 12 harmonics is
/// enough to separate a sine from a stack of edges, and the whole point of the number is that it is
/// FIXED. Changing it changes every colour on every map, which is a deliberate act with the same
/// weight as regenerating a golden.
inline constexpr int kTerrainCycleSamples = 32;
/// HOW MANY PARTIALS AN ADDITIVE TEXEL SUMS: those strictly below the lens's Nyquist (ratio 16 at 32 samples sits on it).
inline constexpr int kAdditiveLensPartials = kTerrainCycleSamples / 2 - 1;
inline constexpr int kTerrainHarmonics = 12;

/// The character at a position, reduced to its three channels. Pure, deterministic, and the same on
/// every machine — no clock, no cache, no state.
///
/// Takes the ENGINE'S OWN PAIR — a zone and a phase offset, where the phase offset already carries
/// gamma and global gamma — rather than the scalar home, because that pair is what actually decides a
/// character and the home alone does not: PhiStair reads its pattern family off the zone.
///
/// AND WHAT THE SOUND'S ROTATIONS BANKED, because §4a's rule is that the map is TRUE. The engine builds
/// every character through `sliceFrameFor(rotate, home, residue)`; a preview built through
/// `sliceBasisFor(rotate, home)` alone is the pre-A26 character — a neighbour of what is being heard,
/// drawn under a star that claims to be it. Empty is the default because empty is what a sound that has
/// never been rotated at a drift carries, and what every fixture written before the fold existed means.
/// `gammaBase` is gamma's unbounded half, and it arrives here rather than inside `phaseOffset` for
/// the reason the engine takes it separately: the offset is a float and the base is a double, and
/// adding them before the call is exactly the quantisation the split exists to avoid.
/// **BOTH TRAVEL COMPONENTS ARE REQUIRED, AND NEITHER GETS A DEFAULT** (backlog GY). The chart's two
/// axes are displacements in the decoded plane, so a call that named only one was drawing the slice
/// at y = 0 — which is precisely the defect this signature change exists to end. A default of 0.0
/// would have kept every existing call compiling and left them all on that slice silently, which is
/// the "a default parameter silences every call site at once" trap this repo has paid for. Twenty
/// call sites now say `0.0` out loud instead, and each one is a place to ask whether it should.
/// WHAT AN ADDITIVE TEXEL READS BEYOND THE PLACE (2026-10-10): the selected sound's Shape Amount and the engine-wide Shape,
/// which pull the place's spectrum toward one band-limited shape as they pull the sound's (design §4.1). Amount 0, the
/// default, is the place's own spectrum. Read only on Additive.
/// AND THE SELECTED SOUND'S SIX BIAS KNOBS (design §4.1), on the place's fields as the bank puts them on the sound's
/// (buildPartialSound): the map shows what this sound's bias makes of each place. Neutral, the default, is the terrain.
struct AdditivePicture {
	feathers::partials::ShapeKind shape = feathers::partials::ShapeKind::Saw;
	float shapeAmount = 0.0f;
	feathers::PartialBias bias {};
	/// THE SELECTED SOUND'S STRING SCALERS (Pluck): the picture reads the place's material through them, as the bank does.
	feathers::StringBias string {};
	/// AND THE SELECTED SOUND'S DRUM (2026-11-05, drum proposal §3.4): its kind (the selected PAD's in Kit mode, the engine's in
	/// Pitched) and its seven scalers; the map draws that kind as heard at each place (drum::heardSpectrum).
	feathers::DrumBias drum {};
	/// AND THE KIT-WIDE TRIMS on top (2026-11-06), the same law again; neutral except on a Drum Kit.
	feathers::DrumBias drumKit {};
	feathers::drum::Kind drumKind = feathers::drum::Kind::Membrane;
	friend bool operator==(const AdditivePicture&, const AdditivePicture&) = default;
};

TerrainFeatures terrainFeaturesAt(OscType engine, const SliceOrientationPin& pin, uint16_t zone,
                                  float phaseOffset, double travelX, double travelY,
                                  const PlaneResidue& residue = kNoPlaneResidue,
                                  double gammaBase = 0.0, feathers::FieldScales scales = {},
                                  feathers::WarpMode warpMode = feathers::WarpMode::Off,
                                  feathers::WeaveModel weaveModel = feathers::WeaveModel::Ring,
                                  const feathers::ResonateShaping& resonateShaping = {},
                                  const feathers::ShapesMix& shapesMix = {}, const AdditivePicture& additive = {});

/// THE PLANE A ROW OF THE MAP IS DRAWN ON, derived once instead of sixteen thousand times.
///
/// M is a function of the turn and the HOME, and a row of the chart is a single home — the chart's
/// vertical axis IS home, and its horizontal is travel, which M does not read. So every texel along a
/// row shares one plane, and deriving it per texel was doing the same 140 triangle reads 128 times
/// over. Hoisting is worth taking for its own sake and it is what keeps M's cost noise: 128 basis
/// builds per map rather than 16,384.
///
/// BOTH ENGINES IN ONE OBJECT because the oscillator type is a parameter a hand can change, and a row
/// that derived only the engine it happens to be drawing would have to derive again the moment the
/// switch moved. They cost a few hundred adds between them.
struct TerrainFrames {
	feathers::SliceBasis<feathers::kMorphTriangles> morph;
	feathers::SliceBasis<feathers::kStairTriangles> stair;
	/// The vowel's own plane. Derived here with the other two rather than per texel for the reason
	/// the row hoist gives above: a frame is ~140 triangle reads and every texel in a row shares one.
	feathers::SliceBasis<feathers::kVoxTriangles> vox;
	/// The Resonate engine's plane, on the same row hoist.
	feathers::SliceBasis<feathers::kWeaveTriangles> weave;
	/// And the additive engine's (2026-10-10), on the same hoist.
	feathers::SliceBasis<feathers::kPartialTriangles> partials;
};

TerrainFrames terrainFramesAt(const SliceOrientationPin& pin, double home,
                              const PlaneResidue& residue);

/// The same features, on a plane the caller has already derived. `home` must be the one the frames
/// were built at — the builder passes both from the same line, so there is no second spelling to get
/// wrong, and `terrainFeaturesAt` is this with the derivation in front of it.
/// THE CYCLE THE BUILDER MAKES AT A PLACE, at `cycle.size()` points — 32 for the terrain's lens, a finer power of two for a
/// like's spectrum. Resonate's pose has 32 nodes and is read between them; Additive sums every partial below the cycle's
/// Nyquist.
void terrainCycleOn(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset, double travelX, double travelY,
                    double gammaBase, feathers::FieldScales scales, feathers::WarpMode warpMode,
                    feathers::WeaveModel weaveModel, const feathers::ResonateShaping& resonateShaping,
                    const feathers::ShapesMix& shapesMix, const AdditivePicture& additive, std::span<float> cycle,
                    const feathers::PhaseWarp* warp = nullptr);
TerrainFeatures terrainFeaturesOn(OscType engine, const TerrainFrames& frames, uint16_t zone,
                                  float phaseOffset, double travelX, double travelY, double gammaBase,
                                  feathers::FieldScales scales = {},
                                  feathers::WarpMode warpMode = feathers::WarpMode::Off,
                                  feathers::WeaveModel weaveModel = feathers::WeaveModel::Ring,
                                  const feathers::ResonateShaping& resonateShaping = {},
                                  const feathers::ShapesMix& shapesMix = {}, const AdditivePicture& additive = {},
                                  float* partialsOut = nullptr, const feathers::PhaseWarp* warp = nullptr);

/// The four channels a feature set produces, each already through its own transfer function.
///
/// EXPOSED SO A TEST CAN MEASURE WHAT THE MAP DRAWS rather than a second copy of the arithmetic, and
/// that is not a convenience — the first version of the calibration test wrote the gains out as
/// literals, so putting one of them back to a value that had ALREADY drawn a broken map left the test
/// green. A fixture that mirrors the implementation's numbers asserts agreement, not correctness.
struct TerrainChannels {
	float luminance = 0.0f; ///< 0..1, from energy
	float hue = 0.0f;       ///< 0..1, the local term from the spectral centroid
	float grain = 0.0f;     ///< 0..1, from roughness — how deep the φ texture cuts
	float ember = 0.0f;     ///< the ridge highlight's depth. A tint on rough, bright ground.
};
/// THE REDUCTION ITSELF, over one cycle of 32 samples — extracted so it can be pointed at a
/// waveform somebody chose rather than only at a place on the map.
///
/// It exists because the thresholds that decide what "siney" MEANS have to be calibrated against
/// known shapes — a sine, a triangle, a square, a sawtooth — and a calibration table quoted from
/// somewhere else is exactly the kind of number that arrives in the wrong units and inverts a
/// conclusion. Measuring the landmarks through THIS function makes the bar checkable rather than
/// relayed. A pure extraction: terrainFeaturesOn calls it and nothing about a texel's colour moved.
///
/// The centroid it returns is DIVIDED BY kTerrainHarmonics - 1, so a sine is 0 and the twelfth
/// harmonic alone would be 1. Any threshold stated in other units is stated in the wrong ones.
/// `partials`, when given, receives the kTerrainHarmonics magnitudes the reduction is made from — the PARTIAL ENVELOPE a
/// like stores and the bake scores against (backlog ON). Null costs nothing.
TerrainFeatures terrainFeaturesOfCycle(std::span<const float> cycle, float* partialsOut = nullptr);

/// `engine` (2026-09-26): the channels are computed from channelFeaturesFor(engine, features) — the features as
/// measured for every engine but Resonate, which reads them quantile-matched onto the shared world. Defaulted
/// to Morph, so every caller that predates it computes what it computed.
TerrainChannels terrainChannels(const TerrainFeatures& features, OscType engine = OscType::Morph);

/// PhiStair's pattern family at a zone: `min(7, zone >> 7)`, which is the engine's own line
/// (phi_stair.cpp) and not a second reading of it.
///
/// Eight real discrete regions — BRICK, TERRACE, RAMP, MESA, PYLON, GLYPH, SHARD, TEETH — at zones
/// 0-127, 128-255, 256-383, 384-511, 512-639, 640-767, 768-895, 896-1023. They are a property of the
/// space rather than a convenience of the picture, which is what §4a means by letting them anchor
/// colour families: the boundary really is a step change in the sound, so a step change in the colour
/// is the honest drawing of it.
int stairFamilyAt(uint16_t zone);

/// How many of them there are, so the anchors spread over the ramp rather than over a guess.
inline constexpr int kStairFamilies = 8;

/// The texel colour those features earn, as opaque ARGB.
///
/// TWO SCALES, MIRRORING THE ENGINE'S OWN, which is what makes the picture legible rather than busy.
/// The long-period ORIENTATION FIELD (aperture, φ-derived from home, drifting over hundreds of zones)
/// sets the colour family — where am I broadly. The fast character measurement sets the local reading
/// within that family — what is it like right here. The engine already has exactly this split: zone
/// gives you a neighbourhood, gamma gives you a country.
/// `stairFamily` anchors the colour family under PhiStair and is negative under PhiMorph, which has
/// no discrete regions to anchor to and takes its family from the orientation field instead.
/// `plane` is the frame this place is drawn on, and it is REQUIRED rather than defaulted for the
/// reason the gamma base is: a caller that forgets it would silently draw the country the way it
/// looked before the landscape turned, and wrongly rather than plainly. A default-constructed frame is
/// a legitimate value meaning "no plane, read the field at home alone", which is what the pre-rotate
/// map did; it is spelled out at the sites that mean it.
/// THE COLOUR THIS PLACE DRAWS, and by design it is the waveform's.
///
/// The waveform owns the picture; the decoding scheme can be revisited if need be. Every channel
/// comes from the character measured at this place — luminance from its energy, hue from its spectral
/// centroid, grain from its roughness. The φ macro field that used to tint it is gone from the draw;
/// it was measured out rather than argued out (it moved a pixel by 1.75 of 255, and its whole range
/// could only ever move one by 21), and the record of why is in the commit history and the ledger.
///
/// `stairFamily` is the one region term that stays, and it is not a field: a stair map anchors its
/// colours on the pattern family `zone >> 7` it is standing in, which §4a rules may honestly mark
/// out the eight discrete regions. Pass −1 for morph, which has no families and is now pure character.
/// THE VIBE PRIOR: a 2-D colour map, and the choice of map is the vibe prior. The axes stay pure measurements of the waveform; the aesthetic identity lives
/// in WHICH map those measurements are read through. So the map is a swappable asset with one place to
/// change it, and picking one is a look decision made from pictures rather than an edit in a formula.
enum class ColourMap {
	Nebula, ///< the cool blue→violet family this surface has always drawn, opened into a plane
	Ember,  ///< a warm map: ash through rust to a hot rim
	Tide,   ///< a cold-to-warm crossing, teal through slate to amber
	// THE ADDITIVE ENGINE'S IDENTITY (retro-futuristic purples, blues and greens): three candidates,
	// brighter and more contrasty than the three above (see terrainGlowFor), chosen per engine by colourMapFor.
	Synthwave, ///< deep indigo, through magenta-violet, to neon cyan
	Aurora,    ///< navy, through teal, to aurora green (violet on the hollow side)
	Crt,       ///< deep purple, through electric blue, to phosphor green
	// THE OTHER ENGINES' IDENTITIES, lifted like the Additive ones (terrainGlowFor), each keeping its hue family:
	NebulaLit, ///< Morph: the Nebula blue/violet/rose, brighter
	Orchid,    ///< Stair: magenta, hot pink and violet (its eight families need hue to separate on)
	Gilt,      ///< Resonate: deep red, amber and gold, to bright yellow
	Spectrum,  ///< Basic Shapes: a full RGB hue sweep along the tilt axis (the second axis softens it toward pastel)
	Grove,     ///< Pluck: walnut, through leaf green, to pale birch (moss on the hollow side)
};

/// WHICH MEASUREMENT THE SECOND AXIS CARRIES. The first is always the spectral centroid — the tilt
/// channel this surface has always coloured by; this is what the map's other axis is fed.
enum class SecondAxis {
	Parity,      ///< odd harmonics' share: hollow against full. The chosen axis.
	Fundamental, ///< the fundamental's own share: pure against developed
	Roughness,   ///< the high-band share, which already drives the grain — the double-booked baseline
};

/// The map that ships, and the axis that feeds it. One place, named, so the vibe is a constant rather
/// than a habit spread across a formula.
inline constexpr ColourMap kShippedMap = ColourMap::Nebula;
inline constexpr SecondAxis kShippedSecondAxis = SecondAxis::Parity;

/// THE MAP ADDITIVE DRAWS: one constant, so choosing between the candidates is a one-word edit.
inline constexpr ColourMap kAdditiveMap = ColourMap::Crt; // chosen from pictures, 2026-10-26
/// PLUCK'S IDENTITY (2026-10-31, backlog MG's per-engine themes): wood and moss, a plucked instrument's colours. Provisional.
inline constexpr ColourMap kPluckMap = ColourMap::Grove;

/// **THE ENGINE SELECTS ITS PALETTE.** Each engine draws its own identity map (Synthwave and Aurora stay as the Additive alternates); the map builder and the
/// upsampler ask here with their window's engine. A palette is a lookup, so this adds nothing per frame.
ColourMap colourMapFor(OscType engine);

/// The glow (brightness) curve a map is drawn through: {floor, range}, so `floor + range * luminance`. The original three keep
/// the mock's `.10 + 1.05*e`; the Additive candidates and the other engines' identity maps lift the floor so quiet country is not black.
struct MapGlow {
	float floor;
	float range;
};
MapGlow terrainGlowFor(ColourMap map);

/// **BOTH COORDINATES ARE TRAVELS, AND THE ORDER IS X THEN Y.** These were `(home, travel)` until
/// backlog GY made the chart a window on the decoded plane; the definition's names moved and this
/// declaration's did not, which is how `upsampleTerrain` came to pass them transposed.
/// `engine` picks the normalisation table the two colour axes are standardised through (featureNormFor):
/// Resonate colours by its own, every other engine by the shared one. Defaulted to Morph so every caller
/// that predates Resonate draws exactly what it drew; the map's builder passes its window's engine.
uint32_t terrainColour(const TerrainFeatures& features, double travelX, double travelY,
                       int stairFamily, ColourMap map = kShippedMap,
                       SecondAxis axis = kShippedSecondAxis,
                       OscType engine = OscType::Morph);


/// The map's own colour at a point of its square, both coordinates already standardised to [0,1].
/// Exposed so the candidate sheets and a case can read the same map the draw does.
struct MapRgb {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};
MapRgb colourMapAt(ColourMap map, float tilt, float second);

/// The tint a stair family carries. Exposed so a case can name the same number the draw uses.
float terrainFamilyTint(int stairFamily);

//-----------//
// The chart //
//-----------//

/// Two lists of likes are the same window input when they hold the same likes; null is none.
inline bool sameLikes(const std::shared_ptr<const std::vector<TerrainLike>>& a,
                      const std::shared_ptr<const std::vector<TerrainLike>>& b) {
	if (a == b) {
		return true;
	}
	const bool aNone = a == nullptr || a->empty();
	const bool bNone = b == nullptr || b->empty();
	if (aNone || bNone) {
		return aNone == bNone;
	}
	return *a == *b;
}

/// **WHAT THE LIKES MAKE OF A WORLD** (backlog ON): the generic derivation (like_surface.h) of the character surface —
/// built on the terrain thread once per world and set of likes, and shared by every map of that world.
using TerrainLikeState = LikeDerivation;

/// The rectangle of character space a chart is drawn over. Position only: no widget, no pixels, no
/// zoom — zoom decides a span and the span is what is stored, so two views at the same span are the
/// same build whatever route the user took to them.
struct TerrainWindow {
	OscType engine = OscType::Morph;
	SliceOrientationPin pin;
	/// What the selected sound's rotations banked. Part of the WINDOW rather than a thing the builder
	/// reads for itself, so that it is part of the key: a fold is a change to every character on the
	/// chart, and a map keyed without it would go on showing the plane the sound has left. Compared by
	/// the defaulted operator== below, which is where "the cache key includes it" actually lives.
	PlaneResidue residue {};
	/// THE GAMMA THE VIEW HAS FOLLOWED — AND THE BUILDER DOES NOT READ IT.
	///
	/// It is here for `carry()` and for `ExploreStage::terrainPlacement()`, the two places that line an
	/// OLD map up against a NEW view. `viewCentre()` follows gamma one for one, so a map built a moment
	/// ago belongs to a view that has since moved; both of them compensate by exactly this difference,
	/// and it is what keeps a gamma sweep from scrolling into nothing.
	///
	/// NOT THE SUM, and the distinction cost a measurement to find: a scrub writes gamma too now, and it
	/// HOLDS ITS VIEW while doing so, so compensating for a scrub's gamma slides the picture off a chart
	/// that never moved — 150px off a 534px widget over ten drag frames, buffer full, zero rebuilds.
	/// So this is every gamma the view actually followed and none that it did not. See
	/// `ExploreStage::viewGamma()`.
	///
	/// **THE SOUND'S OWN HOME, CONSTANT OVER THE WHOLE MAP** (backlog GY: 2026-09-20).
	///
	/// THIS FIELD IS THE REFACTOR. The chart used to draw home down its vertical axis, which made the
	/// vertical display axis the GAMMA axis, and gamma must not correlate with a display axis. Both
	/// axes are now displacements in the 2-space `rotate` decodes, so home is not a
	/// coordinate of the picture at all: it is the one place the whole plane is taken at, and the
	/// Gamma corner is what moves it.
	///
	/// AND IT IS IN THE CONTENT KEY, where `viewGamma` deliberately was not. The old field existed to
	/// re-register a stale map against a view that FOLLOWED gamma one for one; nothing follows gamma
	/// now, so that compensation is gone with it. What replaced it is a real input: move the base and
	/// every texel is a different place, so a window that did not carry it would leave the old map up
	/// and make the corner look dead — the lesson the per-sound pin and the residue each cost an
	/// install to learn.
	double gammaBase = 0.0;
	/// WHERE THE SOUND'S TWO BOUNDED DIALS SIT, and every row of the map is built at exactly these.
	///
	/// A scrub moves the unbounded base and nothing else, so the only thing that differs from row to
	/// row is that base. Building rows at any other zone would draw places this chart cannot take you
	/// to — and under PhiStair it would draw whole pattern FAMILIES a scrub can no longer reach, since
	/// `family = min(7, zone >> 7)` reads the raw dial here exactly as it does on the hardware
	/// (phi_stair.cpp:74-76: gamma arrives through the phase offset and never touches the family).
	double zoneBaseline = 0.0;
	double offsetBaseline = 0.0;
	/// **THE TWO DISPLACEMENTS THE PICTURE IS OF**, and they are the same KIND of quantity — which is
	/// the whole of the design. `travelX` runs across the chart and `travelY` up it; both decode
	/// through the same 2xN basis (`slice_basis.h`: `x[i]*travelX + y[i]*travelY`), so neither is
	/// privileged and no direction in the picture is the gamma direction.
	///
	/// THE ENGINE HAS ALWAYS TAKEN BOTH. `SoundTravel::offset` is an `AnchorPoint` with x and y, both
	/// saved and both decoded (`character_params.h`); the chart simply never wrote y, and the builder
	/// hardcoded `travelY = 0.0`, so the map was a slice of the plane at y = 0 while the star was free
	/// to leave it. That is the defect, and these two fields are where it stops.
	double travelXCentre = 0.0;
	double travelXSpan = 0.0;
	double travelYCentre = 0.0;
	double travelYSpan = 0.0;

	/// THE SELECTED SOUND'S SMOOTHNESS OVERRIDE (doc §11, params.h ASmoothness..HSmoothness), applied
	/// to the smoothness field the flagship builder reads. Set by ExploreStage from the selected
	/// sound's knob; the worker passes it to buildFlagshipSliced, which scales the field BEFORE the
	/// terrain features are measured — so the map shows the scaled (audible) smoothness.
	///
	/// AND IT IS PART OF THE CONTENT KEY, because the builder reads it. It arrived deliberately
	/// EXCLUDED, with a paragraph arguing that a rebuild per modulation tick would thrash the worker —
	/// fifteen lines above the rule that says ANY NEW FIELD THE BUILDER READS MUST BE ADDED HERE BY
	/// HAND, and describing the exact failure that rule was written after paying for three times: the
	/// old map stays up and the verb looks dead. It did: turning Smoothness moved the sound and left
	/// the picture alone, which is half of the reported defect.
	///
	/// The thrash argument does not survive contact with request(): it coalesces, and the worker builds
	/// one map per completion. That is how rotate, gamma and zoom already behave under a drag.
	///
	/// It is NOT the viewGamma case. viewGamma is excluded because the builder does NOT read it — the
	/// rows are built at the sound's own dials — so keying it would ask for a map identical to the one
	/// on screen. The test of the exclusion is what the BUILDER does, never how often the value moves.
	float smoothnessScale = kFieldNeutral;

	/// THE SELECTED SOUND'S FM DEPTH, for the same reason and on the same terms as the scale above:
	/// the builder reads it, so it is in the content key. See the note in terrainFeaturesOn about what
	/// a single-cycle lens can and cannot say about FM.
	float fmDepth = 0.0f;

	/// THE SELECTED SOUND'S FIVE FIELD MODIFIER KNOBS (2026-08-25), on exactly the terms the two
	/// above are on: the builder reads them, so they are in the content key, so turning one repaints
	/// the map. Flat members rather than a nested FieldScales because the paragraph below demands
	/// this key be written out BY HAND, and a nested struct with a defaulted comparison is precisely
	/// the "a new field joins the key for free" shape that paragraph is refusing.
	///
	/// GRIT IS HERE TOO EVEN THOUGH IT IS NOT A FLAGSHIP FIELD. It reaches the morph table, the
	/// builder builds morph tables, and the test of membership is what the BUILDER reads — never
	/// which tier of the engine a value belongs to.
	float quantizeScale = kFieldNeutral;
	float syncScale = kFieldNeutral;
	float feedbackScale = kFieldNeutral;
	float pointScale = kFieldNeutral;
	float gritScale = kFieldNeutral;
	/// WARP, 2026-08-28 — the depth is per sound and the MODE is engine-wide, so both are here and
	/// only one of them is a scale. The mode has to be in this window for the reason the whole
	/// paragraph above exists: the BUILDER reads it, so it belongs to the content key, so changing
	/// the dropdown repaints the map. Without it the terrain would draw unwarped sounds while the
	/// engine played warped ones — flagship.cpp's own warning about the five knobs, one door along,
	/// and the partial-effect shape CLAUDE.md says a difference check passes on.
	///
	/// DEFAULT 0, NOT kFieldNeutral: warpDepth is an amount, not a bias. See dsp/warp.h.
	float warpDepth = 0.0f;
	feathers::WarpMode warpMode = feathers::WarpMode::Off;
	/// THE RESONATE STRING'S MODEL, engine-wide like the warp mode and in the key for the same reason: the
	/// builder reads it (a Resonate texel is the rest pose of THAT model), so switching it repaints the map.
	feathers::WeaveModel weaveModel = feathers::WeaveModel::Ring;
	/// AND RESONATE'S SHAPING (2026-09-26), for the same reason: a Resonate texel is the rest pose of the SHAPED
	/// laws, so Stiffness and Coupling (the two a rest pose depends on) are in the key.
	feathers::ResonateShaping resonateShaping {};
	/// AND THE BASIC SHAPES MIX (2026-10-03): a Basic Shapes texel is the blend read through that place's FM and
	/// warp, so turning a shape knob repaints the map.
	feathers::ShapesMix shapesMix {};
	/// AND THE ADDITIVE SHAPE AND THE SELECTED SOUND'S AMOUNT (2026-10-10): an Additive texel is the place's spectrum pulled
	/// toward that shape, so moving either repaints the map. Set on Additive only.
	/// AND THE SELECTED SOUND'S SIX BIAS KNOBS (AdditivePicture::bias), on the place's fields, for the same reason.
	/// KEYED THROUGH AdditivePicture's DEFAULTED ==, a deliberate exception to the hand-written key below: the struct is
	/// nothing but picture inputs, so a member added to it is in the key by construction rather than by remembering.
	AdditivePicture additive {};
	/// **THE LIKES** (backlog ON): every like the patch holds. In the content key because the builder reads them — they
	/// decide every texel's score. Shared and immutable, compared by CONTENT (`sameLikes`), so a window that carries the
	/// same likes through a new pointer is the same window. Null and empty are the same: no likes.
	std::shared_ptr<const std::vector<TerrainLike>> likes;
	/// **THE SELECTED SOUND'S LIKES' PHASE WARP** (dsp/phase_warp.h) — the same warp the synth builds that sound through,
	/// so every texel is built the way the voice plays it. In the content key (by the warp's content). None is the identity.
	feathers::LikeWarp likeWarp {};
	/// **THE LIKES' STRENGTH** (×1/×2/×4, the heart's menu; stage state, not saved): each like weighs as this many in the
	/// warp's fit. In the key because `deriveLikeWarp` reads it.
	double likeStrength = 1.0;
	/// The fit's other hyperparameters (dsp/phase_warp.h; the shipped values unless a sweep sets them). In the key.
	feathers::PhaseWarpParams likeParams {};

	/// The seven of them as the engine's own carrier, assembled in one place so the builder's two
	/// call sites cannot pass six of seven or transpose two. This is the ONLY conversion between the
	/// window's flat key and FieldScales.
	feathers::FieldScales scales() const {
		return { smoothnessScale, quantizeScale, syncScale, feedbackScale,
		         pointScale,      gritScale,     fmDepth,  warpDepth };
	}

	/// THE CONTENT KEY: every field the builder reads, and ONLY those.
	///
	/// THIS IS WRITTEN OUT BY HAND, WHICH INVERTS A LESSON THIS PROJECT PAID FOR THREE TIMES. The
	/// defaulted comparison was the safe shape precisely because a new field joined the key for free —
	/// the per-sound pin, the residue and the travel centre each cost an install to learn that a window
	/// keyed without something the picture depends on leaves the old map up and the verb looks dead.
	///
	/// It is written out anyway because the opposite fault is now reachable and worse. `gammaSum` and
	/// `gammaBase` are no longer READ by the builder — a scrub moves the base and the rows are built at
	/// the sound's own dials — so with them in the key every drag event would ask for a map that is
	/// identical to the one already on screen, and `carry()` would meanwhile shift the old picture by
	/// the gamma delta and snap it back on publish. A flicker manufactured out of nothing, one gesture
	/// after the terrain stopped needing to change at all.
	///
	/// SO THE EXCLUSION IS GUARDED BY A CASE RATHER THAN BY A COMMENT: "two windows that differ only in
	/// gamma build byte-identical maps" fails the moment that stops being true. ANY NEW FIELD THE
	/// BUILDER READS MUST BE ADDED HERE BY HAND — that is the price of the line above, and the case
	/// cannot collect it for you.
	friend bool operator==(const TerrainWindow& a, const TerrainWindow& b) {
		return a.engine == b.engine && a.pin == b.pin && a.residue == b.residue
		       && a.zoneBaseline == b.zoneBaseline && a.offsetBaseline == b.offsetBaseline
		       && a.gammaBase == b.gammaBase
		       && a.travelXCentre == b.travelXCentre && a.travelXSpan == b.travelXSpan
		       && a.travelYCentre == b.travelYCentre && a.travelYSpan == b.travelYSpan
		       && a.smoothnessScale == b.smoothnessScale && a.fmDepth == b.fmDepth
		       && a.quantizeScale == b.quantizeScale && a.syncScale == b.syncScale
		       && a.feedbackScale == b.feedbackScale && a.pointScale == b.pointScale
		       && a.gritScale == b.gritScale && a.warpDepth == b.warpDepth
		       && a.warpMode == b.warpMode && a.weaveModel == b.weaveModel && a.resonateShaping == b.resonateShaping
		       && a.shapesMix == b.shapesMix && a.additive == b.additive && sameLikes(a.likes, b.likes)
		       && a.likeWarp == b.likeWarp && a.likeStrength == b.likeStrength && a.likeParams == b.likeParams;
	}
};

/// A finished piece of terrain: which window it is of, how much of it exists, and the texels.
///
/// IMMUTABLE, AND THAT IS THE WHOLE SYNCHRONISATION. Nothing ever writes one of these after it is
/// handed out, so a reader holding one cannot be raced by a builder — the three things a frame has to
/// agree about are stapled together and change as one.
///
/// THE THREE RACES THIS REPLACES, kept because each was a different mistake and the third survived the
/// fix for the first two:
///   1. A generation-guarded row-count store could land AFTER `request()` zeroed the count for the
///      next generation, so a stale count outlived the generation it belonged to. A TOCTOU.
///   2. The reader read the window BEFORE acquiring the row count, so the release/acquire edge that
///      was supposed to order the window write never applied to it at all.
///   3. And even with both fixed, a reader already inside a draw keeps reading texel rows while the
///      next generation's builder overwrites them from row 0. Zeroing a count stops the NEXT frame's
///      reader, never the one already running.
/// A seqlock closes 1 and 2 and leaves 3, and still has the reader touching a struct mid-write and
/// discarding it — which is a race whether or not the value is used. An immutable snapshot has no
/// ordering left to get wrong.
struct TerrainSnapshot {
	TerrainWindow window;
	/// Rows this map has BUILT, 0..kResolution. Rows past this are carried or void — see `carried`.
	int rows = 0;
	/// HOW MANY POINTS EACH TEXEL WAS MEANED FROM — 1 for the rough first pass, the schedule's target
	/// once the refined pass has run. Carried so a reader can tell a settled map from a first one, and
	/// so a case can say WHICH of the two it is asserting about rather than racing the builder for it.
	int samples = 1;
	/// Whether the rows past `rows` hold the previous map's texels, resampled to this window's grid,
	/// rather than nothing.
	///
	/// WHY A REBUILD DOES NOT BLANK THE CHART. A window change used to publish its first rows over an
	/// otherwise empty buffer, so every frame drawn during the next 31 ms showed a map that was mostly
	/// void: measured over a held gesture, 104 frames in 120 drew an incomplete map and the average
	/// frame was 43% blank, sawtoothing from 16 rows to 128 and back about thirty times a second. That
	/// is the flicker.
	///
	/// So the builder starts from the map it is replacing rather than from nothing. The carried texels
	/// are placed BY CHART POSITION — the resampling is what makes that true — which keeps the file's
	/// binding rule intact: a carried colour is at the position it was built for, not merely at the
	/// same pixel. Where the two windows overlap and the character function has not changed, a carried
	/// texel is exactly what the builder is about to compute (a re-centre is a pan, and the map is a
	/// pure function of position), so the snap has nothing to redraw. Where the function HAS changed —
	/// a rotation banks a new residue — the new rows sweep down over the old ones, which is a wipe
	/// rather than a flash.
	///
	/// It is not a HELD map. Nothing waits for a build to finish: the seed is published at the instant
	/// the window changes and is overwritten row by row as fast as the rows are computed, so the chart
	/// stays visibly live under the hand.
	bool carried = false;
	std::vector<uint32_t> texels;
	/// **WHICH TEXELS THIS MAP ACTUALLY COMPUTED, as opposed to inherited** — one byte per texel, same
	/// order as `texels`, and the thing that makes a pan's work CUMULATIVE instead of repeated.
	///
	/// MEASURED, and it is the reason this field exists (`FEATHERS_TRACE_TERRAIN_REUSE`, 2026-09-19):
	/// during a drag pan each build could reuse ~90% of the one before it — 14,720 of 16,384 texels,
	/// with the lattice slip reading exactly 0.0000 — and threw every one of them away. So a build
	/// computed sixteen thousand texels to produce about sixteen hundred new ones, took ten times
	/// longer than it needed to, and the next drag frame restarted it. The chart was therefore drawn
	/// from a part-built map on essentially every frame of every drag, which is the reported flicker
	/// that could not be cleared with any of the five debug switches.
	///
	/// `carried` already said a carried texel is "exactly what the builder is about to compute". This
	/// is that sentence made load-bearing: where the carry was EXACT, the texel is marked computed and
	/// the builder skips it.
	///
	/// **ONLY AN EXACT CARRY MAY SET IT.** `carry()` places texels by nearest neighbour, which is an
	/// approximation the moment the two lattices do not coincide — a zoom, a rotation, a dial, a
	/// different pin. Marking a resampled texel as computed would freeze an approximation into the map
	/// permanently, and nothing downstream could tell. So the flag is set only when the windows share
	/// every field but their centres AND their grids are an integer number of texels apart.
	std::vector<uint8_t> computed;
	/// **WHAT EACH TEXEL WAS MEASURED TO BE, kept beside the colour it was turned into** — so a reader can interpolate the
	/// MEASUREMENTS and colour the result, rather than interpolating colours through a ramp that is not a straight line
	/// (the goal: a less pixelated image without sacrificing detail). Same order as `texels`; a texel
	/// nothing has built is zero in both, and `texels` alpha is what says which.
	std::vector<TerrainFeatures> features;
	/// **THE MAP, UPSAMPLED AND COLOURED — BUILT HERE, OFF THE UI THREAD** (`upsampleTerrain`, 2026-09-17).
	/// `fineScale` output pixels per texel each way, or 0 while there is none: it is built ONCE, for the settled map, so a
	/// build's rows still arrive coarse and sharpen when it lands. MEASURED: 40 ms at scale 4, which is why it is not the
	/// editor's to do.
	std::vector<uint32_t> fine;
	int fineScale = 0;

	/// The map's stair pattern family, or -1 under morph — one number for the whole map (the dial picks it; see the
	/// builder), and the other half `terrainColour` needs.
	int family = -1;

	/// **WHAT THE LIKES MAKE OF THIS MAP** (backlog ON) — null without likes. The derived model (and, with it, the warp
	/// the texels were sampled through), shared by every map built in the same world; and each texel's SCORE, how like a
	/// liked sound it is (0..1, 1 at a like's own envelope), in `texels` order, empty without likes. Derived, never stored.
	std::shared_ptr<const TerrainLikeState> likeState;
	std::vector<float> score;
	/// AND, UNDER THE WARP, HOW MANY POINTS EACH TEXEL IS MEANED FROM (its own, from the warp's local stretch), in `texels`
	/// order; empty without likes.
	std::vector<uint8_t> texelSamples;

	uint32_t texel(int x, int y) const {
		return texels[static_cast<size_t>(y) * TerrainMap_kResolution + static_cast<size_t>(x)];
	}
	const TerrainFeatures& featuresAt(int x, int y) const {
		return features[static_cast<size_t>(y) * TerrainMap_kResolution + static_cast<size_t>(x)];
	}
	/// Duplicated from TerrainMap::kResolution, which is not declared yet — see the static_assert
	/// beside that one, which is what stops the two from drifting.
	static constexpr int TerrainMap_kResolution = 128;
};
/// HOW MUCH WIDER THE PREFETCHED MAP IS than the view it was built beside — the ground a pan walks onto before a build
/// can land (a low-priority pre-emptive render, to make panning smoother). Same texel
/// count, so it is this much coarser; it is carried from, never drawn.
/// HOW MANY OUTPUT PIXELS PER TEXEL the settled map is upsampled to — see `TerrainSnapshot::fine`. Four is about one
/// output pixel per screen pixel at the editor's scale, and 16x the texels' memory (1 MB a map).
inline constexpr int kFineScale = 4;

inline constexpr double kPrefetchSpread = 1.8;

/// HOW LONG THE TERRAIN MUST GO UNASKED before it spends anything on the wider map — long enough that a hand still moving
/// is never behind it, short enough that the pause between two drags is enough to have one.
inline constexpr int kPrefetchQuietMs = 250;

/// **THE MAP, UPSAMPLED — INTERPOLATE THE MEASUREMENTS, THEN COLOUR** (cubic interpolation, the goal a
/// less pixelated image without sacrificing detail).
///
/// `scale` output pixels per texel each way, into `out`, which must hold `(kResolution * scale)^2` BGRA texels. Each
/// output pixel takes CATMULL-ROM over the four nearest texels each way — an INTERPOLATING cubic, so it passes exactly
/// through every texel the builder measured and invents only what is between them, where a smoothing cubic would blur
/// what was measured. It can also put a peak BETWEEN two samples where the slopes say the ground is still climbing on
/// both sides, which is the one way an upsample recovers anything; that is deliberate and is why the taps are not
/// clamped to their neighbours.
///
/// **THE FEATURES ARE WHAT IS INTERPOLATED, NOT THE COLOURS.** A colour is the ramp's answer to a measurement and the
/// ramp is not a straight line, so mixing two colours can land on a colour the ramp never contains. Each output pixel is
/// coloured by `terrainColour` at its OWN position, which also evaluates the grain — a function of position — per pixel
/// rather than per texel, so the grain is finer than the grid rather than blocky with it.
///
/// **THE VOID IS NOT INTERPOLATED ACROSS.** A texel nothing has built has no measurement, so it is left out of the taps
/// and the weights are renormalised over what is there; an output pixel with no built texel near it is transparent, as
/// its texel was. MESSAGE THREAD: it allocates nothing, but it is not for the audio thread or the builder.
void upsampleTerrain(const TerrainSnapshot& snapshot, int scale, uint32_t* out, size_t outCount);

/// **THE ONE HOME A WINDOW'S WHOLE PLANE IS TAKEN AT** — the builder's own `planeHome`, so a like can say which plane it
/// was made on and the editor can ask whether the map being drawn is that plane.
double terrainPlaneHome(const TerrainWindow& window);

/// **A STORED PLACE'S FULL POINT** (backlog ON: places stored as plane (x, y) did not follow a rotate).
/// The point (x, y) of `window`'s plane decodes to, in the engine's whole triangle space: for every frame the engine reads
/// (Morph: morph; Stair: stair; V5: morph, stair, vox; Basic Shapes: morph, stair; Resonate: weave; Additive: partials),
/// each triangle's phase (`SliceBasis::phaseFor` at the plane's home) and its rate row (residue plus travel, what
/// `rateFor` adds to 1). That is what the sound at the place IS, so it is what a like stores. Returns how many rows it wrote
/// (at most kTerrainLikePoint); 0 on an engine with no plane.
size_t terrainPlacePoint(const TerrainWindow& window, double x, double y, std::span<double> out);

/// WHERE A STORED POINT LANDS ON `window`'s PLANE: the place on it nearest the point (least squares over the same rows),
/// and how far off the plane the point is (in travel units, against the plane's own scale) — reported, never used to hide
/// a mark (by design no fade; a place draws at its projection wherever that lands, and off the chart it is clipped).
/// EXACT when the plane is the one the point was decoded on: (`x`, `y`) decoded here giving the point back to the bit means
/// this IS that plane, and (`x`, `y`) are returned as they are.
struct PlanePlace {
	double x = 0.0;
	double y = 0.0;
	double offPlane = 0.0;
	bool exact = false;
};
PlanePlace terrainPlaceOn(const TerrainWindow& window, std::span<const double> point, double x, double y);
/// A like's place on `window`'s plane (its stored point; a like stored without one is where it was stored).
PlanePlace terrainLikePlaceOn(const TerrainWindow& window, const TerrainLike& like);
/// **THE PARTIAL ENVELOPE AT ONE PLACE OF A WINDOW'S WORLD** (backlog ON) — the twelve magnitudes the bake measures for a
/// texel centred there, through the builder's own road (its dials, its plane, every field the window carries), at one
/// sample. What a like stores at the moment it is made.
std::array<float, kTerrainHarmonics> terrainPartialsAt(const TerrainWindow& window, double travelX, double travelY);
/// **THE FINE SPECTRUM AT ONE PLACE** (backlog ON's Hellinger fingerprint): the cycle the builder makes there, at `samples`
/// points rather than the lens's 32, in the likes' sixth-octave bins. What a like stores and the warp's points are scored by.
LikeSpectrum terrainFineSpectrumAt(const TerrainWindow& window, double travelX, double travelY, int samples = kLikeFineCycle);

/// THE CONTROLS THE PRE-PASS NUDGES, by name, in `LikeModel::consistency`'s order.
std::span<const char* const> terrainLikeControls();
/// **THE DERIVATION** (backlog ON): the likes' region, an 8x8 pre-pass over it at the window's controls and with each
/// control nudged in turn, and the model built from that and the likes' stored envelopes. Pure: a function of the
/// window's world (never its view) and its likes. Null without likes. Terrain thread, or a test.
std::shared_ptr<const TerrainLikeState> deriveTerrainLikes(const TerrainWindow& window);
/// **WHETHER THE LIKES MOVE THE PLANE** (the warp above). OFF since P-MÖBIUS shipped (2026-10-28): the likes reshape the
/// SOUND — every place's angles, the voice's and the bake's alike (dsp/phase_warp.h) — and the plane is drawn where it is,
/// as auditioned it. The warp's code and cases stay; the map simply does not derive it.
inline constexpr bool kLikesMoveThePlane = false;
/// **THE SELECTED SOUND'S PHASE WARP** (P-MÖBIUS, backlog ON): each like's ANGLES as heard, captured when it was made
/// (TerrainLike::angles), fitted site by site on the circle at the window's strength (dsp/phase_warp.h `buildPhaseWarp`).
/// Nothing here reads the plane, so a rotate, a gamma or a knob cannot retarget a like. None without likes. Every engine has
/// sites (`likeLiveSites`).
feathers::LikeWarp deriveLikeWarp(const TerrainWindow& window);
/// The sites an engine's voice reads (dsp/phase_warp.h's flat layout): V5 its morph table, operator authors and vowel;
/// Basic Shapes the table and authors; Morph the table; Additive its partials; Stair its table; Resonate its string's laws.
std::array<bool, feathers::kAngleSites> likeLiveSites(OscType engine);
/// A PLACE'S PHASE-WARP ANGLES: every site the voice reads there, in dsp/phase_warp.h's flat layout, recorded from the
/// builders themselves. What a like captures.
void terrainLikeAngles(const TerrainWindow& window, double x, double y, std::array<float, kTerrainLikeAngles>& out);
/// **THE REFINE** (like_surface.h `refineLikes`) of a derivation for the same window: the distant centres searched coarse to
/// fine and every lens shaped by the score. Null when `abandon` said to give up. Terrain thread, when nothing is waiting.
std::shared_ptr<const TerrainLikeState> refineTerrainLikes(const TerrainWindow& window, const TerrainLikeState& base,
                                                         const std::function<bool()>& abandon);

/// A window's worth of terrain, built on a thread that is not the editor's and never the audio
/// thread.
///
/// PROGRESSIVE. A build is tens of milliseconds; the surface draws the rows that exist and leaves the
/// rest as void, so a zoom is a map filling in rather than a frame that arrives late. The builder
/// publishes a fresh snapshot every few rows — a 64KB copy about eight times a build, microseconds
/// against a 31 ms build — rather than mutating one the editor might be reading.
class TerrainMap {
public:
	TerrainMap();
	~TerrainMap();
	TerrainMap(const TerrainMap&) = delete;
	TerrainMap& operator=(const TerrainMap&) = delete;

	/// Texels per side. Fixed rather than derived from the widget, because a map that changed
	/// resolution with the window would be a different picture of the same place.
	static constexpr int kResolution = 128;

	/// Ask for a window. Cheap and idempotent: asking for the window already being built or already
	/// built does nothing at all, which is what lets the surface call it every frame.
	///
	/// ALSO WHERE THE WORKER IS BORN. An editor that never shows Explore never starts a thread, and the
	/// surface is what asks first.
	void request(const TerrainWindow& window);

	/// The terrain as it stands, for one frame to draw from. Never null.
	///
	/// TAKE IT ONCE AND KEEP IT for the whole of a draw. Calling twice is not wrong, but the two can be
	/// different maps, and a frame that mixed one map's window with another's texels would be drawing a
	/// colour at coordinates it does not belong to — which is the exact failure the snapshot exists to
	/// make unrepresentable.
	std::shared_ptr<const TerrainSnapshot> snapshot() const;

	/// How many WINDOWS have been asked for, and how many maps have actually been started. Two counters
	/// because the difference between them is the coalescing: a held gesture asks every frame and the
	/// builder starts one map per completion, so `requests()` climbing while `builds()` crawls IS the
	/// throttle working. One counter could not tell those apart.
	uint64_t requests() const noexcept { return generation_.load(std::memory_order_relaxed); }
	/// How many builds the WORKER has started.
	///
	/// INCREMENTED BY THE WORKER, NOT BY request() — which matters to anyone counting. A count taken
	/// straight after asking for a window reads whatever the thread has got to, so a case that
	/// captures a baseline immediately and compares it a moment later sees 0 against 1 and reads it as
	/// a spurious rebuild. That is a FALSE POSITIVE, and the expensive direction: it sends someone
	/// hunting a rebuild that never happened. Settle first, then count.
	uint64_t builds() const noexcept { return buildsStarted_.load(std::memory_order_relaxed); }
	/// **HOW MANY TEXELS THE BUILDER HAS ACTUALLY COMPUTED**, across every build — the seam that makes
	/// the reuse assertable without asserting a TIMING.
	///
	/// IT EXISTS BECAUSE THE FIRST ATTEMPT ASSERTED A CLOCK. "No frame of this gesture sees a
	/// part-built map" is true on a quiet box and false at load 29, where the builder simply cannot
	/// keep up — so the case went red for a reason that had nothing to do with the code, which is the
	/// microsecond-bound trap this repo already pays for twice in `vox-voice` and `flagship`.
	///
	/// A COUNT OF WORK DONE IS NOT A CLOCK. A pan whose windows coincide on the lattice must compute
	/// only the newly exposed strip however slow or busy the machine is, because the skip is decided
	/// by `TerrainSnapshot::computed` and not by how much time was available. That is the claim worth
	/// pinning, and it is load-independent by construction.
	uint64_t texelsComputed() const noexcept {
		return texelsComputed_.load(std::memory_order_relaxed);
	}
	/// HOW MANY WIDER MAPS HAVE BEEN PREFETCHED (`prefetched_`) — a test's way to wait for one, since the thing itself is
	/// never drawn and a picture cannot show it.
	uint64_t prefetches() const noexcept { return prefetches_.load(std::memory_order_relaxed); }

	/// **EVERY SNAPSHOT THE VIEW IS HANDED, IN ORDER — A TEST SEAM, and it exists because sampling cannot
	/// see a publish.** Called on the terrain thread straight after each publish, with the snapshot just
	/// published.
	///
	/// WHY A SAMPLE IS NOT ENOUGH: "a rebuild does not blank the chart" is a claim about what EACH
	/// rebuild publishes first, and a case that looks at `snapshot()` once a frame sees whichever publish
	/// the host's speed happened to put in front of it. MEASURED 2026-09-23 at load 43: the same gesture
	/// read 1.6% mean void on a quiet box and 19% here, with no code between them — the frame count was a
	/// clock, and it was measuring the machine. Seen from here, every rebuild's first publish can be held
	/// against the map it replaced, whatever the machine is doing.
	///
	/// SET IT BEFORE THE FIRST `request()` OR NOT AT ALL: it is read by the worker without a lock, and
	/// the worker's birth is what orders the write before those reads. A call after that is ignored.
	using PublishObserver = std::function<void(const std::shared_ptr<const TerrainSnapshot>&)>;
	void observePublishesForTest(PublishObserver observer);

	/// **WHETHER THE SETTLED MAP IS UPSAMPLED BY THE CUBIC — LIVE, FROM THE EDITOR** (a debug toggle,
	/// so the choice can be made by eye).
	///
	/// THE CUBIC WAS REMOVED ON A MEASUREMENT THAT COULD NOT SEE WHAT IT WAS FOR. Two STILL frames,
	/// cubic against the atlas's own bilinear, RMSE 1.21 of 255 — and stillness is exactly the condition
	/// under which the difference does not arise. At 128 texels magnified about four times, a sub-pixel
	/// PAN resamples every texel edge differently on every frame; at 512 the magnification is near 1:1
	/// and it does not. Whether that is the flicker on the chart is a question for an eye, so this is a
	/// switch rather than a second opinion from me.
	///
	/// DEFAULT FALSE — what is installed today — so turning it ON is the experiment and OFF is the
	/// control. Atomic because the editor writes it and the terrain thread reads it; relaxed because a
	/// map built a frame either side of the flip is a map either way, and nothing downstream cares which.
	void setFineWanted(bool wanted) { fineWanted_.store(wanted, std::memory_order_relaxed); }
	[[nodiscard]] bool fineWanted() const noexcept { return fineWanted_.load(std::memory_order_relaxed); }

	/// Whether the worker exists yet. An editor that never shows Explore should never have started one,
	/// and "never started" is a property worth being able to ask about rather than infer.
	bool workerStarted() const;

	/// THE MOST QUADS THE TERRAIN MAY SUBMIT IN ONE FRAME, and it is a hard limit of the renderer
	/// rather than a taste.
	///
	/// visage's shape batcher allocates transient buffers and writes its indices as `uint16_t`, with
	/// the vertex index for quad `i` being `4 * i` (shape_batcher.cpp, `initTransientQuadBuffers`).
	/// So quad 16384 addresses vertices 65536..65539, which WRAP to 0..3 — that quad silently draws
	/// over the first quad's vertices, and the result is garbage geometry confined to wherever those
	/// shapes were. There is no cap in visage and no error: it just wraps.
	///
	/// The map is 128x128 = 16384 texels, and at a normal zoom about 12,900 of them fall inside the
	/// disk, comfortably under. But a snapshot built for a SMALLER window than the view — zoom in, then
	/// back out, which is exactly the recipe that reproduced this — draws its texels crowded toward the
	/// centre, and then ALL 16384 are inside the disk. One over.
	static constexpr int kMaxTerrainQuads = 16000;

	/// How often the builder publishes while a map fills in, in ROWS — a row is the unit it finishes,
	/// and milliseconds would make the cadence depend on the machine.
	///
	/// DO NOT "OPTIMISE" THIS AWAY, and here is the arithmetic so nobody has to guess. A snapshot is
	/// 128x128x4 = 64KB. At 16 rows that is 8 copies per build, 512KB of memcpy against a build that
	/// costs 31 ms of character evaluation — around 0.1% of it, and far below the 16 ms a frame has.
	/// Publishing per row would be 128 copies for the same picture; publishing only at the end would
	/// cost the progressive fill, which is what stops a zoom from arriving as a late frame.
	///
	/// The copy is not overhead to be shaved: it is what makes the handover safe. A builder that
	/// mutated the map the editor is holding is the entire class of bug this replaced.
	static constexpr int kPublishEvery = 16;

	/// Where a texel's centre sits in chart coordinates. The one conversion, so the picture and the
	/// scrub cannot disagree about which place a pixel is.
	/// WHERE THE SAMPLING LATTICE ACTUALLY SITS: the window's centre snapped to a whole number of cells,
	/// so the grid belongs to the WORLD rather than to the view. Public because a case has to be able to
	/// ask where a sample really was, and because the answer is the reason browsing stopped re-rolling
	/// the picture — see the note at the definition for what was seen and what it measured.
	static double anchoredTravelY(const TerrainWindow& window);
	static double anchoredTravelX(const TerrainWindow& window);
	static double travelYAtRow(const TerrainWindow& window, double row);
	static double travelXAtColumn(const TerrainWindow& window, double column);

	/// And back: which row and column of a window a chart position lands on. The exact inverses of the
	/// two above, and they exist so that carrying a map into a new window is a POSITION lookup rather
	/// than an index offset — an offset would be right only for a pan and silently wrong for a zoom.
	static double rowAtTravelY(const TerrainWindow& window, double home);
	static double columnAtTravelX(const TerrainWindow& window, double travel);

	/// Place `from`'s texels into `into` by chart position, and say whether anything landed.
	///
	/// PUBLIC SO A TEST CAN MEASURE THE PLACEMENT DIRECTLY rather than inferring it from a build in
	/// flight — a race is a bad place to check arithmetic. Nearest texel, because the two grids do not
	/// generally line up and a carried texel is replaced within one build anyway; what matters is that
	/// it is carried to the POSITION it was built for, which is the rule that lets it be drawn at all.
	/// Positions `from` does not cover, and rows it had not built, are left alone.
	/// Place `from`'s texels into `into` by chart position. `onlyEmpty` leaves whatever is already there — which is what
	/// the prefetched map is carried under, behind the map being replaced.
	static bool carry(TerrainSnapshot& into, const TerrainSnapshot& from, bool onlyEmpty = false);

private:
	/// One map, built — see the definition. TERRAIN THREAD ONLY.
	std::shared_ptr<const TerrainSnapshot> buildMap(const TerrainWindow& window,
	                                                const std::shared_ptr<const TerrainSnapshot>& previous,
	                                                bool publishing);
	void worker();
	void publish(std::shared_ptr<const TerrainSnapshot> snapshot);

	/// The one thing both threads touch, and only ever under the mutex: a shared_ptr the builder
	/// replaces and the editor copies. Copying it is the whole of a reader's synchronisation.
	std::shared_ptr<const TerrainSnapshot> current_;
	/// **THE WIDER MAP BUILT WHEN NOTHING WAS WAITING** (`kPrefetchSpread`) — never drawn, only carried from, so a pan
	/// exposes a softer version of the country rather than void. TERRAIN THREAD ONLY: the editor never sees it.
	std::shared_ptr<const TerrainSnapshot> prefetched_;
	/// **THE LIKES' DERIVATION, KEPT FOR THE WORLD IT WAS MADE IN** (backlog ON) — the window it was derived from with its
	/// view (centres, spans) cleared, so a pan or a zoom reuses it and anything else the builder reads derives it again.
	/// Terrain thread only.
	TerrainWindow likeKey_;
	std::shared_ptr<const TerrainLikeState> likeState_;
	/// `observePublishesForTest` — written only before the worker exists, read only by it.
	PublishObserver publishObserver_;

	TerrainWindow wanted_; ///< guarded by mutex_
	std::atomic<bool> stop_ { false };
	std::atomic<uint64_t> generation_ { 0 };
	std::atomic<uint64_t> buildsStarted_ { 0 };
	std::atomic<uint64_t> prefetches_ { 0 };
	std::atomic<bool> fineWanted_ { false };
	std::atomic<uint64_t> texelsComputed_ { 0 };

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	bool haveWork_ = false;
	bool started_ = false; ///< whether the worker has been born yet — see request()
	std::thread thread_;
};

static_assert(TerrainSnapshot::TerrainMap_kResolution == TerrainMap::kResolution,
              "the snapshot's stride and the map's resolution are the same number");

} // namespace feathers::ui
