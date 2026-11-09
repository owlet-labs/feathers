/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/registry.h"
#include "fx/automod/automod_activity.h"
#include "fx/automod/automod_phase_space.h"
#include "fx/automod/automod_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

// TWIN FIELD — EIGHT MODULATION OUTPUTS READ OFF AUTOMOD'S φ TERRAIN, IN TWO LAYERS, CROSSFADED.
// The design in one breath: each layer
// reads AutoMod's terrain (the same `activity` its pad draws) at eight points on a ring around a puck;
// the puck orbits its pan point at AutoMod's rate; Mod i = (1 - x) A_i + x B_i, with Solo A / Solo B
// overriding the blend and both soloed meaning neither.
namespace feathers::mod::twinfield {

inline constexpr size_t kPoints = 8;
inline constexpr size_t kLayers = 2;

/// ONE LAYER'S CONTROLS, in order. A layer's block is `kLayerParams` long and starts at `layerParam`.
enum class LayerParam : size_t {
	PanX = 0,
	PanY,
	Zoom,
	/// THE RING'S SIZE ON THE TERRAIN, in zones — its own control since zoom and radius were decoupled (see `ringRadius`).
	Spread,
	Rotate,
	Gamma,
	Rate,
	Sync,
	Mode,
	Depth,
	kCount,
};
inline constexpr size_t kLayerParams = static_cast<size_t>(LayerParam::kCount);

enum class Param : size_t {
	Crossfade = 0,
	SoloA,
	SoloB,
	LayerA,                                  ///< the first of A's block
	LayerB = LayerA + kLayerParams,          ///< the first of B's block
	Enabled = LayerB + kLayerParams,
	kCount,
};

[[nodiscard]] constexpr size_t layerParam(size_t layer, LayerParam p) noexcept {
	return static_cast<size_t>(Param::LayerA) + layer * kLayerParams + static_cast<size_t>(p);
}
[[nodiscard]] constexpr size_t at(Param p) noexcept { return static_cast<size_t>(p); }

/// THE MOTION MODES — AutoMod's LFO modes less Stop, which hands AutoMod's modulation to a Manual knob
/// this device does not have. Depth 0 is the still puck.
enum class Mode : int {
	Free,      ///< runs continuously; locked to the song while the transport plays
	Retrigger, ///< restarts the orbit on each note
	Once,      ///< one orbit per note, then holds
	kCount,
};
inline constexpr const char* kModeNames[] = { "Free", "Retrigger", "Once" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

//-- THE GEOMETRY ---------------------------------------------------------------------------------//

/// THE PAD'S HALF-VIEW AT ZOOM 1, in plane units — AutoMod's pad: `kZonesPerHalfDrag / kZonesShown`, one
/// zone. The card draws the terrain at this scale, so the ring below lands where the card draws its dots.
inline constexpr double kHalfViewPlane = 1.0 / 8.0;
/// **ONE ZONE, IN PLANE UNITS** — Spread's unit. AutoMod's `kOneZone`, and the pad's half-view at zoom 1.
inline constexpr double kZonePlane = kHalfViewPlane;
/// **SPREAD'S DEFAULT, 0.6 ZONES — THE RING THE FIRST BUILD READ AT ITS DEFAULT ZOOM**, `0.6 * kHalfViewPlane / 2^0`. Not
/// to the bit: a row stores 0.6 as a FLOAT (0.60000002), so the radius moves by 4e-8 relative. The case "Spread's
/// default reads where the zoom-coupled ring read at default zoom" measures what that does to the eight outputs.
inline constexpr double kSpreadDefaultZones = 0.6;
inline constexpr double kSpreadMaxZones = 4.0;
/// THE HOME THE PLANE IS TAKEN AT: AutoMod's default coordinate (Type 0.30, Flavour 0.70, Mod 0.10).
inline constexpr double kHomeType = 0.30;
inline constexpr double kHomeFlavour = 0.70;
inline constexpr double kHomeMod = 0.10;

inline constexpr double kPanMax = 8.0;
inline constexpr double kZoomMinOct = -2.0; ///< AutoMod's pad 0.25x
inline constexpr double kZoomMaxOct = 6.0;  ///< AutoMod's pad 64x
inline constexpr double kRotateMax = 4.0;
inline constexpr double kGammaMax = 8.0;

//-- THE PARAMS -----------------------------------------------------------------------------------//

#define FEATHERS_TWINFIELD_LAYER(L, LETTER, GAMMA_DEF)                                                              \
	{ .key = L ".pan_x", .name = "Pan X " LETTER, .min = -kPanMax, .max = kPanMax, .def = 0.0, .stepped = false,     \
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,                                                 \
	  .tooltip = "Where layer " LETTER "'s puck sits across the terrain — drag the pad to move it. Click the pad and the wheel zooms there (Ctrl/Cmd+wheel always does)", \
	  .modSpan = 0.25, .centred = true, .shortName = "Pan X" },                                                         \
	{ .key = L ".pan_y", .name = "Pan Y " LETTER, .min = -kPanMax, .max = kPanMax, .def = 0.0, .stepped = false,     \
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,                                                 \
	  .tooltip = "Where layer " LETTER "'s puck sits up the terrain — drag the pad to move it. Click the pad and the wheel zooms there (Ctrl/Cmd+wheel always does)",  \
	  .modSpan = 0.25,                                                                                                \
	  .centred = true, .shortName = "Pan Y" },                                                                          \
	{ .key = L ".zoom", .name = "Zoom " LETTER, .min = kZoomMinOct, .max = kZoomMaxOct, .def = 0.0, .stepped = false, \
	  .unit = device::Unit::Number, .names = {}, .modulatable = false,                                                \
	  .tooltip = "How close layer " LETTER "'s terrain is viewed, in octaves — the picture only; Spread sizes the ring. "  \
	             "A clicked pad zooms on the wheel too, and Ctrl/Cmd+wheel always does",                             \
	  .shortName = "Zoom" },                                                                                            \
	{ .key = L ".spread", .name = "Spread " LETTER, .min = 0.0, .max = kSpreadMaxZones, .def = kSpreadDefaultZones,     \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                               \
	  .tooltip = "How far apart layer " LETTER "'s eight points read the terrain, in zones — its own size, whatever "  \
	             "the zoom shows",                                                                                    \
	  .shortName = "Spread" },                                                                                          \
	{ .key = L ".rotate", .name = "Rotate " LETTER, .min = -kRotateMax, .max = kRotateMax, .def = 0.0,                \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                               \
	  .tooltip = "Turns the plane layer " LETTER " is sliced through the phase space — the same pan reads new "      \
	             "ground",                                                                                            \
	  .centred = true, .shortName = "Rotate" },                                                                         \
	{ .key = L ".gamma", .name = "Gamma " LETTER, .min = 0.0, .max = kGammaMax, .def = GAMMA_DEF, .stepped = false, \
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,                                                 \
	  .tooltip = "Moves every triangle's phase of layer " LETTER " together — AutoMod's Gamma, in laps",             \
	  .shortName = "Gamma" },                                                                                           \
	{ .key = L ".rate", .name = "Rate " LETTER, .min = -2.0, .max = 2.0, .def = 0.0, .stepped = false,               \
	  .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,                                            \
	  .tooltip = "Scales the orbit rate the Sync list selected — 1X is 1:1", .shortName = "Rate" },                  \
	{ .key = L ".sync", .name = "Sync " LETTER, .min = 0.0,                                                          \
	  .max = static_cast<double>(fx::automod::kSyncChoiceCount) - 1.0, .def = 0.0, .stepped = true,                  \
	  .unit = device::Unit::Choice, .names = fx::automod::kSyncNames, .modulatable = false,                          \
	  .tooltip = "A free rate, or a division that locks layer " LETTER "'s orbit to the host's tempo",              \
	  .shortName = "Sync" },                                                                                            \
	{ .key = L ".mode", .name = "Mode " LETTER, .min = 0.0, .max = static_cast<double>(Mode::kCount) - 1.0,          \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kModeNames, .modulatable = false,          \
	  .tooltip = "Free runs (locked to the song while it plays); Retrigger restarts on a note; Once orbits once "    \
	             "per note",                                                                                          \
	  .shortName = "Mode" },                                                                                            \
	{ .key = L ".depth", .name = "Depth " LETTER, .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,              \
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,                                                \
	  .tooltip = "How far layer " LETTER "'s puck orbits, as a share of the ring; 0 holds it still",                \
	  .shortName = "Depth" }

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "crossfade", .name = "Crossfade", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Blends the eight OUTPUTS: 0 is layer A's, 100% is layer B's — not a place between them" },
	{ .key = "solo_a", .name = "Solo A", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Outputs layer A alone, ignoring the crossfade. With both soloed, the crossfade applies" },
	{ .key = "solo_b", .name = "Solo B", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Outputs layer B alone, ignoring the crossfade. With both soloed, the crossfade applies" },
	FEATHERS_TWINFIELD_LAYER("a", "A", 0.0),
	// B'S GAMMA DEFAULTS HALF A LAP AWAY: two identical layers at load would make the crossfade a dead knob.
	FEATHERS_TWINFIELD_LAYER("b", "B", 0.5),
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Bypass switch — off: every output rests at 0" },
};
#undef FEATHERS_TWINFIELD_LAYER
static_assert(std::size(kParamDescs) == at(Param::kCount));

inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "mod_1", .name = "Mod 1" }, { .key = "mod_2", .name = "Mod 2" },
	{ .key = "mod_3", .name = "Mod 3" }, { .key = "mod_4", .name = "Mod 4" },
	{ .key = "mod_5", .name = "Mod 5" }, { .key = "mod_6", .name = "Mod 6" },
	{ .key = "mod_7", .name = "Mod 7" }, { .key = "mod_8", .name = "Mod 8" },
};
static_assert(std::size(kOutputs) == kPoints);

//-- THE SETTINGS, AND THE READING AS A PURE FUNCTION OF THEM ------------------------------------//

struct LayerSettings {
	double panX = 0.0;
	double panY = 0.0;
	double zoomOct = 0.0;
	double spread = kSpreadDefaultZones; ///< zones
	double rotate = 0.0;
	double gamma = 0.0;
	float rate = 0.0f;
	int sync = 0;
	Mode mode = Mode::Free;
	double depth = 0.5;
	friend bool operator==(const LayerSettings&, const LayerSettings&) = default;
};

struct Settings {
	double crossfade = 0.5;
	bool soloA = false;
	bool soloB = false;
	std::array<LayerSettings, kLayers> layers {};
	bool enabled = true;
	friend bool operator==(const Settings&, const Settings&) = default;
};

/// THE ROW, decoded — clamped to the declared ranges, a non-finite field taking its default.
[[nodiscard]] inline Settings settingsOf(const float* fields, size_t count) {
	const auto field = [&](size_t i) {
		const device::ParamDesc& d = kParamDescs[i];
		const double v = (fields != nullptr && i < count && std::isfinite(fields[i])) ? fields[i] : d.def;
		return std::clamp(v, d.min, d.max);
	};
	Settings s;
	s.crossfade = field(at(Param::Crossfade));
	s.soloA = field(at(Param::SoloA)) >= 0.5;
	s.soloB = field(at(Param::SoloB)) >= 0.5;
	s.enabled = field(at(Param::Enabled)) >= 0.5;
	for (size_t l = 0; l < kLayers; ++l) {
		LayerSettings& ls = s.layers[l];
		ls.panX = field(layerParam(l, LayerParam::PanX));
		ls.panY = field(layerParam(l, LayerParam::PanY));
		ls.zoomOct = field(layerParam(l, LayerParam::Zoom));
		ls.spread = field(layerParam(l, LayerParam::Spread));
		ls.rotate = field(layerParam(l, LayerParam::Rotate));
		ls.gamma = field(layerParam(l, LayerParam::Gamma));
		ls.rate = static_cast<float>(field(layerParam(l, LayerParam::Rate)));
		ls.sync = static_cast<int>(std::lround(field(layerParam(l, LayerParam::Sync))));
		ls.mode = static_cast<Mode>(std::lround(field(layerParam(l, LayerParam::Mode))));
		ls.depth = field(layerParam(l, LayerParam::Depth));
	}
	return s;
}

/// AND BACK — the row a `Settings` came from (for the range sweep's round trip, and the tests).
[[nodiscard]] inline std::array<float, at(Param::kCount)> rowOf(const Settings& s) {
	std::array<float, at(Param::kCount)> row {};
	row[at(Param::Crossfade)] = static_cast<float>(s.crossfade);
	row[at(Param::SoloA)] = s.soloA ? 1.0f : 0.0f;
	row[at(Param::SoloB)] = s.soloB ? 1.0f : 0.0f;
	row[at(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	for (size_t l = 0; l < kLayers; ++l) {
		const LayerSettings& ls = s.layers[l];
		row[layerParam(l, LayerParam::PanX)] = static_cast<float>(ls.panX);
		row[layerParam(l, LayerParam::PanY)] = static_cast<float>(ls.panY);
		row[layerParam(l, LayerParam::Zoom)] = static_cast<float>(ls.zoomOct);
		row[layerParam(l, LayerParam::Spread)] = static_cast<float>(ls.spread);
		row[layerParam(l, LayerParam::Rotate)] = static_cast<float>(ls.rotate);
		row[layerParam(l, LayerParam::Gamma)] = static_cast<float>(ls.gamma);
		row[layerParam(l, LayerParam::Rate)] = ls.rate;
		row[layerParam(l, LayerParam::Sync)] = static_cast<float>(ls.sync);
		row[layerParam(l, LayerParam::Mode)] = static_cast<float>(static_cast<int>(ls.mode));
		row[layerParam(l, LayerParam::Depth)] = static_cast<float>(ls.depth);
	}
	return row;
}

/// THE PLANE ONE LAYER IS READ ON — AutoMod's: the home pushed by gamma along all 39 triangles, and the
/// basis `sliceBasisFor` derives from the rotate generator at that home. A function of (rotate, gamma).
struct Plane {
	fx::automod::phase_space::Phases home {};
	fx::automod::phase_space::Basis basis {};
};

[[nodiscard]] inline Plane planeFor(double rotate, double gamma) {
	namespace ps = fx::automod::phase_space;
	Plane p;
	p.home = ps::homeFor(kHomeType, kHomeFlavour, kHomeMod);
	ps::pushAlong(p.home, ps::kGammaAxis, gamma);
	p.basis = ps::basisFor(rotate, ps::planeHomeOf(p.home));
	return p;
}

/// **THE RING'S RADIUS IN PLANE UNITS — `spread` zones, AND NOTHING ELSE.**
///
/// It was `0.6 * kHalfViewPlane / 2^zoom`, fixed on the glass so zoom spread the points across the terrain. On audition the zoom/radius coupling felt wrong, so the two are DECOUPLED: Zoom
/// is the picture's scale only, and the ring is sized on the TERRAIN by Spread. A zoomed-in pad therefore draws the
/// ring larger on the glass. **No "couple to zoom" switch**: it would add control bloat, and a
/// macro routed to both Zoom and Spread gives the coupling anyway.
[[nodiscard]] inline double ringRadius(double spreadZones) {
	return spreadZones * kZonePlane;
}

/// WHERE THE PUCK IS, relative to its pan point, at an orbit phase (turns): a circle of `depth` rings.
[[nodiscard]] inline std::array<double, 2> orbitOffset(double depth, double spreadZones, double phase) {
	const double r = depth * ringRadius(spreadZones);
	const double a = 2.0 * std::numbers::pi * phase;
	return { r * std::cos(a), r * std::sin(a) };
}

/// POINT k's offset from the puck: Mod 1 east, counter-clockwise (Y up) in 45 degree steps.
[[nodiscard]] inline std::array<double, 2> pointOffset(size_t k, double spreadZones) {
	const double r = ringRadius(spreadZones);
	const double a = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(kPoints);
	return { r * std::cos(a), r * std::sin(a) };
}

/// THE TERRAIN AT A PLANE POINT, 0..1 — AutoMod's pad's brightness, through the shared fast road.
[[nodiscard]] inline float terrainAt(const Plane& plane, double x, double y) {
	namespace ps = fx::automod::phase_space;
	return fx::automod::activityAtPhases(ps::phasesAt(plane.home, plane.basis, x, y));
}

/// WHERE ONE LAYER'S EIGHT POINTS READ, in plane (terrain) units, at an orbit phase. Zoom is not an input.
[[nodiscard]] inline std::array<std::array<double, 2>, kPoints> pointCoords(const LayerSettings& l, double phase) {
	const auto puck = orbitOffset(l.depth, l.spread, phase);
	std::array<std::array<double, 2>, kPoints> out {};
	for (size_t k = 0; k < kPoints; ++k) {
		const auto o = pointOffset(k, l.spread);
		out[k] = { l.panX + puck[0] + o[0], l.panY + puck[1] + o[1] };
	}
	return out;
}

/// ONE LAYER'S EIGHT READINGS at an orbit phase. Pure: the device, the card and the tests all call it.
[[nodiscard]] inline std::array<float, kPoints> readLayer(const Plane& plane, const LayerSettings& l,
                                                          double phase) {
	const auto at = pointCoords(l, phase);
	std::array<float, kPoints> out {};
	for (size_t k = 0; k < kPoints; ++k) {
		out[k] = terrainAt(plane, at[k][0], at[k][1]);
	}
	return out;
}

/// THE BLEND — and the solos. Solo A alone: A. Solo B alone: B. Neither, or BOTH: the crossfade (a
/// mixer's solos are additive, so soloing both hears both; there is no hidden third state).
[[nodiscard]] inline float blend(float a, float b, double crossfade, bool soloA, bool soloB) {
	if (soloA && !soloB) {
		return a;
	}
	if (soloB && !soloA) {
		return b;
	}
	const float x = static_cast<float>(crossfade);
	return (1.0f - x) * a + x * b;
}

inline constexpr const char* kTags[] = { "terrain", "phi", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "TFIELD",
	.name = "Twin Field",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.outputGridColumns = 4,
};

extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::twinfield
